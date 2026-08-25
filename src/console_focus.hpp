#pragma once

namespace big
{
	// RoM's logger calls AllocConsole, and a freshly created console takes
	// foreground focus. Hades pauses when it loses focus, so if the console
	// wins the race against the game's own window appearing, the game sits
	// paused at startup: process alive and responding, CPU flat, and
	// ScriptManager::Update never fires. It looks exactly like a hang.
	//
	// This waits for the game window to exist and, if the console still holds
	// the foreground, hands it back. Blocks the calling thread, so call it last
	// on late_init.
	//
	// No-op when the console is disabled, and it will not take focus away from
	// a window belonging to another process - if the user has alt-tabbed away
	// on purpose, that is left alone.
	void hand_console_focus_back_to_game();
} // namespace big
