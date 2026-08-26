#include "common.hpp"

#include "config/config.hpp"
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

	void draw_mods_tab()
	{
		const auto& modules = big::g_lua_manager->m_modules;
		ImGui::TextUnformatted(std::format("{} mod(s) loaded", modules.size()).c_str());
		ImGui::Separator();

		if (ImGui::BeginTable("mods", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Mod");
			ImGui::TableSetupColumn("Version");
			ImGui::TableHeadersRow();

			for (const auto& mod : modules)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(mod->guid().c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(mod->manifest().version_number.c_str());
			}

			ImGui::EndTable();
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
	void draw_mod_gui_elements()
	{
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

		if (const HMODULE user32 = GetModuleHandleA("user32.dll"))
		{
			if (const auto clip_cursor = GetProcAddress(user32, "ClipCursor"))
			{
				g_clip_cursor_hook =
				    safetyhook::create_inline(reinterpret_cast<void*>(clip_cursor), reinterpret_cast<void*>(&clip_cursor_detour));

				LOG(INFO) << (g_clip_cursor_hook ? "Hooked ClipCursor so the overlay can free the mouse."
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

			    if (g_is_open)
			    {
				    draw_mods_window();
			    }

			    if (was_open && !g_is_open)
			    {
				    toggle(false);
			    }
		    });
	}
} // namespace big::gui
