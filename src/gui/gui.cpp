#include "common.hpp"

#include "config/config.hpp"
#include "file_manager/file_manager.hpp"
#include "gui.hpp"
#include "hades1/hades_lua.hpp"
#include "hades1/script_hook.hpp"
#include "lua/lua_manager.hpp"
#include "lua_extensions/lua_manager_extension.hpp"
#include "lua/bindings/gui.hpp"
#include "renderer.hpp"
#include "version.hpp"

#include <safetyhook.hpp>

// Toggle behaviour follows Hell2Modding's gui::toggle / toggle_mouse
// (hell2-reference/src/gui/gui.cpp): hand the mouse to ImGui and stop the game
// confining the cursor. Two deliberate departures:
//
//   - **No V-Sync change.** Theirs drops V-Sync while the overlay is open. On
//     Hades 1 that uncaps the framerate and the fans audibly spin up, so it is
//     not copied. Their D3D12 present path presumably needs it; D3D11 does not.
//   - They patch ClipCursor in the *executable's* import table, because Hades 2
//     keeps its engine in the exe. Ours lives in EngineWin64s.dll, so an inline
//     hook on user32!ClipCursor is simpler and catches every caller.

namespace
{
	bool g_is_open        = false;

	SafetyHookInline g_clip_cursor_hook{};

	toml_v2::config_file::config_entry<int>* g_toggle_key = nullptr;
	toml_v2::config_file::config_entry<bool>* g_mod_gui_callbacks = nullptr;
	toml_v2::config_file::config_entry<bool>* g_onboarding_shown  = nullptr;

	// Non-null while the user is picking a new key for that entry.
	toml_v2::config_file::config_entry_base* g_rebinding = nullptr;

	// A virtual-key code as a human sees it. "45" means nothing to a player.
	//
	// GetKeyNameTextW wants a *scan* code in bits 16-23, and the extended-key
	// bit in bit 24. Without that bit Insert, Delete, Home, End, the arrows
	// and the navigation cluster all report their numpad twins - INSERT comes
	// out as "Num 0", which is worse than showing the number.
	std::string key_name(int vk)
	{
		if (vk <= 0 || vk > 254)
		{
			return "(none)";
		}

		switch (vk)
		{
		case VK_LBUTTON: return "Mouse Left";
		case VK_RBUTTON: return "Mouse Right";
		case VK_MBUTTON: return "Mouse Middle";
		case VK_XBUTTON1: return "Mouse 4";
		case VK_XBUTTON2: return "Mouse 5";
		default: break;
		}

		UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
		if (!scan)
		{
			return std::format("Key {}", vk);
		}

		switch (vk)
		{
		case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
		case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
		case VK_UP:     case VK_DOWN:   case VK_DIVIDE: case VK_NUMLOCK:
			scan |= 0x100;
			break;
		default:
			break;
		}

		wchar_t buffer[64]{};
		const int length = GetKeyNameTextW(static_cast<LONG>(scan << 16), buffer, static_cast<int>(std::size(buffer)));
		if (length <= 0)
		{
			return std::format("Key {}", vk);
		}

		const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, length, nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(bytes), 0);
		WideCharToMultiByte(CP_UTF8, 0, buffer, length, out.data(), bytes, nullptr, nullptr);
		return out;
	}

	// Clear every key's "pressed since last queried" bit.
	//
	// GetAsyncKeyState's low bit is set if the key went down at any point
	// since *that key* was last queried - and on the very first query for a
	// key, that window reaches back to process start. So without this the
	// first poll after entering rebind mode reports the mouse click that
	// entered it, and the hotkey silently becomes Mouse Left.
	void flush_key_state()
	{
		for (int vk = 1; vk <= 254; ++vk)
		{
			GetAsyncKeyState(vk);
		}
	}

	// The first key pressed since the previous call, or 0.
	int poll_pressed_key()
	{
		for (int vk = 1; vk <= 254; ++vk)
		{
			// Mouse buttons are never accepted. The overlay is operated with
			// the mouse, so a mouse-bound toggle fires while you are using the
			// UI, and binding it to left or right leaves the config
			// unreachable from inside the game - there is no way to click your
			// way back and fix it.
			if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
			{
				continue;
			}

			if (GetAsyncKeyState(vk) & 1)
			{
				return vk;
			}
		}

		return 0;
	}

	BOOL WINAPI clip_cursor_detour(const RECT* rect)
	{
		// sgg::App::UpdateCursorLock re-clips the cursor every frame, so a
		// one-off ClipCursor(nullptr) on open would be undone immediately.
		// Neutering the call itself is what actually frees the mouse.
		if (g_is_open)
		{
			rect = nullptr;
		}

		return g_clip_cursor_hook.call<BOOL, const RECT*>(rect);
	}

	void toggle_mouse()
	{
		// The ImGui context does not exist until the first present, and the
		// overlay can be toggled before then.
		if (!ImGui::GetCurrentContext())
		{
			return;
		}

		auto& io = ImGui::GetIO();

		if (g_is_open)
		{
			io.MouseDrawCursor  = true;
			io.ConfigFlags     &= ~ImGuiConfigFlags_NoMouse;
			io.ConfigFlags     &= ~ImGuiConfigFlags_NoMouseCursorChange;

			ClipCursor(nullptr);
		}
		else
		{
			io.MouseDrawCursor  = false;
			io.ConfigFlags     |= ImGuiConfigFlags_NoMouse;
			io.ConfigFlags     |= ImGuiConfigFlags_NoMouseCursorChange;
		}
	}

	// The overlay does not pause the game - it freezes the player the same way
	// the game's own screens do. FreezePlayerUnit / UnfreezePlayerUnit
	// (UIScripts.lua:2542) key off a named flag in
	// CurrentRun.Hero.FreezeInputKeys, so ours composes with any freeze the game
	// already has in place, and controls are only restored once every flag is
	// gone. Both early-return when there is no active run, so calling them at
	// the main menu is harmless.
	//
	// These are Lua calls into the game's state, so they must go through the
	// script thread rather than run here on the render thread.
	void set_player_frozen(bool frozen)
	{
		big::hades1::run_on_script_thread(
		    [frozen]
		    {
			    if (!big::lua_manager_extension::is_lua_state_valid())
			    {
				    return;
			    }

			    sol::state_view state(big::g_lua_manager->lua_state());

			    sol::protected_function fn = state[frozen ? "FreezePlayerUnit" : "UnfreezePlayerUnit"];
			    if (!fn.valid())
			    {
				    return;
			    }

			    const auto result = fn("Hell1Modding");
			    if (!result.valid())
			    {
				    LOG(ERROR) << "Freeze/Unfreeze failed: " << result.get<sol::error>().what();
			    }
		    });
	}

	void toggle(bool open)
	{
		// Hell2Modding drops V-Sync while its overlay is open. Do NOT copy that
		// here: with V-Sync off Hades 1 renders flat out and the framerate goes
		// uncapped the moment the overlay opens - measurably, the fans spin up.
		// Their D3D12 present path presumably needs it; our D3D11 one does not.

		const bool changed = (open != g_is_open);

		g_is_open = open;
		toggle_mouse();

		if (changed)
		{
			set_player_frozen(open);
		}
	}

	// Draw one config entry as the right widget for its type, and persist the
	// file when it changes.
	//
	// config_entry_base stores its value in a std::any, so the type switch is
	// on type(). Anything unrecognised is shown read-only rather than silently
	// skipped - a mod using a type we do not handle should still be visible.
	void draw_config_entry(toml_v2::config_file* file, toml_v2::config_file::config_entry_base* entry)
	{
		const auto& key   = entry->m_definition.m_key;
		const auto& type  = entry->type();
		bool changed      = false;

		ImGui::PushID(entry);

		// The toggle key is the one setting every player touches, and as a
		// bare InputInt it reads "Toggle Key [45]", which is meaningless
		// without Microsoft's virtual-key table open in another window.
		if (entry == static_cast<toml_v2::config_file::config_entry_base*>(g_toggle_key))
		{
			const int current = entry->get_value_base<int>();

			if (g_rebinding == entry)
			{
				ImGui::Button("Press a key...  (Esc to cancel)");

				if (const int pressed = poll_pressed_key())
				{
					if (pressed != VK_ESCAPE)
					{
						entry->set_value_base<int>(pressed);
						changed = true;
					}

					g_rebinding = nullptr;
				}
			}
			else if (ImGui::Button(key_name(current).c_str()))
			{
				g_rebinding = entry;

				// Discard the click that just got us here, and anything else
				// pressed earlier in the session.
				flush_key_state();
			}

			ImGui::SameLine();
			ImGui::TextUnformatted(key.c_str());

			// A way back if the bound key turns out to be unreachable.
			ImGui::SameLine();
			if (ImGui::SmallButton("Reset"))
			{
				entry->set_value_base<int>(VK_INSERT);
				changed     = true;
				g_rebinding = nullptr;
			}

			ImGui::PopID();

			if (changed)
			{
				file->save();
			}

			return;
		}

		if (type == typeid(bool))
		{
			bool value = entry->get_value_base<bool>();
			if (ImGui::Checkbox(key.c_str(), &value))
			{
				entry->set_value_base<bool>(value);
				changed = true;
			}
		}
		else if (type == typeid(int))
		{
			int value = entry->get_value_base<int>();
			if (ImGui::InputInt(key.c_str(), &value))
			{
				entry->set_value_base<int>(value);
				changed = true;
			}
		}
		else if (type == typeid(int64_t))
		{
			auto value    = entry->get_value_base<int64_t>();
			int as_int    = static_cast<int>(value);
			if (ImGui::InputInt(key.c_str(), &as_int))
			{
				entry->set_value_base<int64_t>(static_cast<int64_t>(as_int));
				changed = true;
			}
		}
		else if (type == typeid(double))
		{
			auto value = static_cast<float>(entry->get_value_base<double>());
			if (ImGui::InputFloat(key.c_str(), &value))
			{
				entry->set_value_base<double>(static_cast<double>(value));
				changed = true;
			}
		}
		else if (type == typeid(std::string))
		{
			auto value = entry->get_value_base<std::string>();
			char buffer[512]{};
			std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
			if (ImGui::InputText(key.c_str(), buffer, sizeof(buffer)))
			{
				entry->set_value_base<std::string>(buffer);
				changed = true;
			}
		}
		else
		{
			ImGui::TextDisabled("%s (unsupported type)", key.c_str());
		}

		// The description is the same text that appears as a comment in the
		// .cfg, so it is already written for a human.
		if (!entry->m_description.m_description.empty() && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", entry->m_description.m_description.c_str());
		}

		ImGui::PopID();

		if (changed)
		{
			// Write through immediately. A crash between edit and exit would
			// otherwise lose the change, and these files are small.
			file->save();
		}
	}

	void draw_config_file(const char* label, toml_v2::config_file* file)
	{
		if (!file || file->count() == 0)
		{
			return;
		}

		if (!ImGui::CollapsingHeader(label))
		{
			return;
		}

		ImGui::Indent();

		// m_entries is keyed by config_definition, which sorts by section then
		// key, so walking it in order groups sections naturally.
		std::string current_section;

		for (auto& [definition, entry] : file->m_entries)
		{
			if (definition.m_section != current_section)
			{
				current_section = definition.m_section;
				ImGui::SeparatorText(current_section.c_str());
			}

			draw_config_entry(file, entry.get());
		}

		ImGui::Unindent();
	}

	// Folders under plugins/ that hold a manifest but never became a module.
	//
	// RoMBase keeps a mod that *failed* to load in m_modules (deliberately, so
	// it can be hot-reloaded once fixed), but a mod blocked by a missing
	// dependency is never constructed at all - it is dropped during the
	// topological sort and only mentioned in a log line. Without this, such a
	// mod is simply absent from the UI with no explanation, which is the most
	// confusing failure a first-time user can hit. Comparing the folder
	// against the loaded set is the only way to see them.
	std::vector<std::string> find_unloaded_mods()
	{
		std::vector<std::string> unloaded;

		std::error_code ec;
		const auto plugins = big::g_file_manager.get_project_folder("plugins").get_path();

		for (const auto& entry : std::filesystem::directory_iterator(plugins, ec))
		{
			if (ec || !entry.is_directory(ec))
			{
				continue;
			}

			if (!std::filesystem::exists(entry.path() / "manifest.json", ec))
			{
				continue;
			}

			const auto folder = entry.path().filename().string();

			const bool loaded = std::any_of(big::g_lua_manager->m_modules.begin(),
			                                big::g_lua_manager->m_modules.end(),
			                                [&](const auto& mod)
			                                {
				                                return mod->guid() == folder;
			                                });

			if (!loaded)
			{
				unloaded.push_back(folder);
			}
		}

		return unloaded;
	}

	void draw_mods_tab()
	{
		const auto& modules = big::g_lua_manager->m_modules;

		size_t failing = 0;
		for (const auto& mod : modules)
		{
			failing += (mod->m_error_count > 0) ? 1 : 0;
		}

		ImGui::TextUnformatted(std::format("{} mod(s) loaded", modules.size()).c_str());
		if (failing)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "- %zu reporting errors", failing);
		}
		ImGui::Separator();

		if (ImGui::BeginTable("mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Mod");
			ImGui::TableSetupColumn("Version");
			ImGui::TableSetupColumn("Errors");
			ImGui::TableHeadersRow();

			for (const auto& mod : modules)
			{
				const bool has_errors = mod->m_error_count > 0;

				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				if (has_errors)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", mod->guid().c_str());
				}
				else
				{
					ImGui::TextUnformatted(mod->guid().c_str());
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(mod->manifest().version_number.c_str());

				ImGui::TableNextColumn();
				if (has_errors)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu", mod->m_error_count);
				}
				else
				{
					ImGui::TextDisabled("-");
				}
			}

			ImGui::EndTable();
		}

		// Rescanning the folder every frame would hit the disk in the present
		// hook; the set only changes when the manager is rebuilt.
		static std::vector<std::string> unloaded;
		static size_t last_module_count = SIZE_MAX;
		if (last_module_count != modules.size())
		{
			last_module_count = modules.size();
			unloaded          = find_unloaded_mods();
		}

		if (!unloaded.empty())
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%zu mod(s) present but not loaded:", unloaded.size());

			for (const auto& guid : unloaded)
			{
				ImGui::BulletText("%s", guid.c_str());
			}

			ImGui::TextWrapped("Usually a missing dependency. LogOutput.log names it: look for \"Can't load ... because it's missing ...\".");
		}

		if (ImGui::Button("Open mods folder"))
		{
			const auto path = big::g_file_manager.get_project_folder("plugins").get_path();
			ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		ImGui::SameLine();

		if (ImGui::Button("Open log folder"))
		{
			const auto path = big::g_file_manager.get_project_file("./LogOutput.log").get_path().parent_path();
			ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	void draw_config_tab()
	{
		ImGui::TextWrapped("Changes are written to the .cfg files immediately. Some take effect on the next launch.");
		ImGui::Separator();

		draw_config_file("Hell1Modding", big::config::general.get());

		// Every mod may own config files of its own, created through the
		// config.config_file Lua API. They are edited exactly the same way.
		for (const auto& mod : big::g_lua_manager->m_modules)
		{
			for (const auto& file : mod->m_data.m_config_files)
			{
				draw_config_file(mod->guid().c_str(), file.get());
			}
		}
	}

	// Shown once, ever. Without it a player who has not read the README has no
	// way to discover the overlay exists: there is no on-screen affordance,
	// and with the console disabled there is nothing telling them the loader
	// is even running.
	void draw_onboarding()
	{
		if (!g_onboarding_shown || g_onboarding_shown->get_value())
		{
			return;
		}

		static bool opened = false;
		if (!opened)
		{
			opened = true;
			ImGui::OpenPopup("Welcome to Hell1Modding");
		}

		ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);

		if (!ImGui::BeginPopupModal("Welcome to Hell1Modding", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			return;
		}

		ImGui::TextWrapped("Hell1Modding is running. Your mods are loaded and this overlay is how you check on them.");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextUnformatted("Open and close this overlay with:");
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", key_name(g_toggle_key ? g_toggle_key->get_value() : VK_INSERT).c_str());
		ImGui::TextWrapped("You can change that under the Config tab. While it is open you cannot move or attack, but the game keeps running.");

		ImGui::Spacing();
		ImGui::TextWrapped("Mods go in the plugins folder, one folder per mod. The Mods tab lists what loaded and flags anything reporting errors.");

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button("Got it", ImVec2(120.0f, 0.0f)))
		{
			g_onboarding_shown->set_value(true);
			big::config::general->save();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void draw_mods_window()
	{
		if (!ImGui::Begin("Hell1Modding", &g_is_open))
		{
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(std::format("v{} ({})", big::version::VERSION_NUMBER, big::version::GIT_BRANCH).c_str());
		ImGui::Separator();

		// The manager is rebuilt every load wave and destroyed on lua_close, so
		// this can legitimately be absent - say so rather than showing nothing.
		std::scoped_lock lock(big::lua_manager_extension::g_manager_mutex);

		if (!big::lua_manager_extension::is_lua_state_valid())
		{
			ImGui::TextUnformatted("Lua state not up yet.");
			ImGui::End();
			return;
		}

		if (ImGui::BeginTabBar("hell1modding_tabs"))
		{
			if (ImGui::BeginTabItem("Mods"))
			{
				draw_mods_tab();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Config"))
			{
				draw_config_tab();
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	// RoMBase already gives mods gui.add_imgui, gui.add_always_draw_imgui and
	// gui.add_to_menu_bar, and stores what they register on each module. It
	// never draws them - that is the fork's job, and is why nothing appeared
	// until now despite the API being live and documented.
	// Run the ImGui callbacks mods registered through gui.add_imgui and
	// friends.
	//
	// **This executes mod Lua on the render thread, and that is not safe.**
	// Hades runs its own Lua from the main thread (ScriptManager::Update) and
	// from worker threads (World::PostLoad -> Script::run); the engine's own
	// THREAD_GUARD only logs when it notices. Driving one lua_State from the
	// present hook at the same time corrupts the VM.
	//
	// Observed on a live install: SGG_Modding-ReLoad registers an
	// always-draw callback purely as a per-frame tick, so this ran every
	// frame. Roughly a second into gameplay the process took two faults 8ms
	// apart - one on sgg::renderThread inside raw_imgui_callback::draw with
	// luaV_execute in *our* Lua, one on a worker thread inside Script::run
	// with luaV_execute in the *engine's* - preceded by
	// "attempt to index a thread value" from Main.lua's coroutine scheduler
	// finding its own state trampled.
	//
	// g_manager_mutex does not help: it stops us racing ourselves, and the
	// engine has never heard of it.
	//
	// Off by default until the Lua entry points are serialised properly. The
	// fix is to hook the engine's lua_pcallk - every one of the three crash
	// stacks passes through it - and hold a recursive lock for its duration,
	// which the present hook also takes before calling into Lua.
	void draw_mod_gui_elements()
	{
		if (!g_mod_gui_callbacks || !g_mod_gui_callbacks->get_value())
		{
			return;
		}

		std::scoped_lock lock(big::lua_manager_extension::g_manager_mutex);

		if (!big::lua_manager_extension::is_lua_state_valid())
		{
			return;
		}

		for (const auto& mod : big::g_lua_manager->m_modules)
		{
			// Always-draw elements render whether or not the overlay is open,
			// which is how a mod puts a permanent HUD on screen.
			for (const auto& element : mod->m_data.m_always_draw_independent_gui)
			{
				element->draw();
			}

			if (!g_is_open)
			{
				continue;
			}

			for (const auto& element : mod->m_data.m_independent_gui)
			{
				element->draw();
			}
		}

		if (!g_is_open)
		{
			return;
		}

		if (ImGui::BeginMainMenuBar())
		{
			for (const auto& mod : big::g_lua_manager->m_modules)
			{
				for (const auto& element : mod->m_data.m_menu_bar_callbacks)
				{
					element->draw();
				}
			}

			ImGui::EndMainMenuBar();
		}
	}

} // namespace

namespace big::gui
{
	bool is_open()
	{
		return g_is_open;
	}

	void init()
	{
		g_toggle_key = config::general->bind("GUI",
		                                     "Toggle Key",
		                                     VK_INSERT,
		                                     "Virtual-key code that opens and closes the Hell1Modding overlay. "
		                                     "Defaults to Insert (0x2D). See Microsoft's Virtual-Key Codes list.");

		g_onboarding_shown = config::general->bind("GUI",
		                                           "Onboarding Shown",
		                                           false,
		                                           "Set once the first-run welcome window has been dismissed. "
		                                           "Set it back to false to see it again.");

		g_mod_gui_callbacks = config::general->bind("GUI",
		                                            "Mod Lua Gui Callbacks",
		                                            false,
		                                            "Run ImGui callbacks that mods register (gui.add_imgui and "
		                                            "friends). Off by default: these execute mod Lua on the render "
		                                            "thread while the game is running Lua on its own threads, which "
		                                            "corrupts the shared lua_State and crashes shortly into "
		                                            "gameplay. Only turn this on to develop against the API.");

		if (const HMODULE user32 = GetModuleHandleA("user32.dll"))
		{
			if (const auto clip_cursor = GetProcAddress(user32, "ClipCursor"))
			{
				g_clip_cursor_hook =
				    safetyhook::create_inline(reinterpret_cast<void*>(clip_cursor), reinterpret_cast<void*>(&clip_cursor_detour));

				LOG(DEBUG) << (g_clip_cursor_hook ? "Hooked ClipCursor so the overlay can free the mouse."
				                                 : "safetyhook refused ClipCursor; the mouse will stay locked to the window.");
			}
		}

		// RoMBase exposes gui.is_open() / gui.toggle() to mods but leaves the
		// implementation to the fork. Point them at our overlay so a mod asking
		// "is the menu open" gets the truth.
		lua::gui::g_on_is_open = []
		{
			return g_is_open;
		};

		lua::gui::g_on_toggle = []
		{
			toggle(!g_is_open);
		};

		add_renderer_draw_callback(
		    []
		    {
			    // Edge-triggered: GetAsyncKeyState's low bit is set once per
			    // press, so holding the key does not flap the window.
			    if (g_toggle_key && (GetAsyncKeyState(g_toggle_key->get_value()) & 1))
			    {
				    toggle(!g_is_open);
			    }

			    // Begin() writes straight to g_is_open via its close button, so
			    // catch that and run the same teardown as the hotkey.
			    const bool was_open = g_is_open;

			    // Mod-registered ImGui runs regardless: always-draw elements are
			    // meant to be visible with the overlay closed.
			    draw_mod_gui_elements();

			    // First run: open the overlay ourselves so the welcome window
			    // has mouse input and the player can see it at all. Doing this
			    // before draw_mods_window keeps the ordering sane - the popup
			    // is modal over the overlay behind it.
			    if (g_onboarding_shown && !g_onboarding_shown->get_value() && !g_is_open)
			    {
				    toggle(true);
			    }

			    if (g_is_open)
			    {
				    draw_mods_window();
				    draw_onboarding();
			    }

			    if (was_open && !g_is_open)
			    {
				    toggle(false);
			    }
		    });
	}
} // namespace big::gui
