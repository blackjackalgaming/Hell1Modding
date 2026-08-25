#pragma once

#include <functional>

namespace big::hades1
{
	// Installs the inline hook on sgg::ScriptManager::Update. Must run after
	// resolve_known_symbols(). Returns false if the address is unresolved or
	// safetyhook refuses the target.
	bool install_script_hook();

	// Queues work to run inside the next Update, i.e. on the engine's script
	// thread. This is the only sanctioned way to touch Lua or call
	// ScriptManager::Load: both are single-threaded by contract, and the
	// engine's THREAD_GUARD only logs the violation rather than preventing it,
	// so calling them from our own thread appears to work right up until it
	// corrupts the VM.
	//
	// Safe to call before the hook is installed - tasks queue up and run on the
	// first Update. Tasks run once, in the order queued.
	void run_on_script_thread(std::function<void()> task);
} // namespace big::hades1
