#include "common.hpp"

#include "config/config.hpp"
#include "gui.hpp"
#include "hades1/hades_lua.hpp"
#include "hades1/script_hook.hpp"
#include "lua/lua_manager.hpp"
#include "lua_extensions/lua_manager_extension.hpp"
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

		ImGui::End();
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
