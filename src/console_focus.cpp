#include "common.hpp"

#include "console_focus.hpp"

namespace
{
	struct find_window_context
	{
		DWORD pid       = 0;
		HWND console    = nullptr;
		HWND found      = nullptr;
	};

	// The game's own top-level window: same process, visible, not the console,
	// and not an owned popup (splash screens and dialogs are owned).
	HWND find_game_window()
	{
		find_window_context ctx{GetCurrentProcessId(), GetConsoleWindow(), nullptr};

		EnumWindows(
		    [](HWND window, LPARAM param) -> BOOL
		    {
			    auto* ctx = reinterpret_cast<find_window_context*>(param);

			    DWORD window_pid = 0;
			    GetWindowThreadProcessId(window, &window_pid);

			    if (window_pid != ctx->pid || window == ctx->console || !IsWindowVisible(window)
			        || GetWindow(window, GW_OWNER) != nullptr)
			    {
				    return TRUE; // keep looking
			    }

			    ctx->found = window;
			    return FALSE; // stop
		    },
		    reinterpret_cast<LPARAM>(&ctx));

		return ctx.found;
	}
} // namespace

namespace big
{
	void hand_console_focus_back_to_game()
	{
		const HWND console = GetConsoleWindow();
		if (!console)
		{
			// Console Enabled = false, so nothing ever stole focus.
			return;
		}

		// The window does not exist at DLL attach - the renderer creates it a
		// few seconds in. 60s is far longer than that, and costs nothing when
		// the window turns up early.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

		while (std::chrono::steady_clock::now() < deadline)
		{
			if (const HWND game_window = find_game_window())
			{
				// Only reclaim if the console is still what is focused. If the
				// user has clicked the game already, or switched to another
				// application entirely, leave their choice alone.
				if (GetForegroundWindow() == console)
				{
					SetForegroundWindow(game_window);
					LOG(INFO) << "Console had the foreground; handed it back to the game window.";
				}

				return;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		LOG(WARNING) << "Game window never appeared; leaving console focus alone.";
	}
} // namespace big
