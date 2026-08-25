#include "common.hpp"

#include "config_options.hpp"
#include "game_pdb.hpp"
#include "hades_lua.hpp"
#include "lua_extensions/lua_manager_extension.hpp"
#include "script_hook.hpp"

#include <safetyhook.hpp>

// The hook on sgg::ScriptManager::Update exists for one reason: it is the only
// place we are allowed to touch Lua from.
//
// Update is `void Update(const float& dt)` and, like everything else on
// ScriptManager, is static (SA in the mangled name), so there is no `this` -
// the reference arrives in RCX as a plain const float*.

namespace
{
	SafetyHookInline g_update_hook{};
	SafetyHookInline g_load_hook{};
	SafetyHookInline g_lua_close_hook{};
	SafetyHookInline g_init_lua_hook{};
	SafetyHookInline g_lua_pcallk_hook{};

	std::mutex g_task_mutex;
	std::vector<std::function<void()>> g_tasks;

	void run_pending_tasks()
	{
		// Swap the queue out under the lock rather than running tasks while
		// holding it: a task is free to queue more work, and doing that from
		// inside the drain would deadlock.
		std::vector<std::function<void()>> tasks;
		{
			std::lock_guard lock(g_task_mutex);
			if (g_tasks.empty())
			{
				return;
			}
			tasks.swap(g_tasks);
		}

		for (auto& task : tasks)
		{
			try
			{
				task();
			}
			catch (const std::exception& e)
			{
				LOG(ERROR) << "Script-thread task threw: " << e.what();
			}
			catch (...)
			{
				LOG(ERROR) << "Script-thread task threw a non-std exception.";
			}
		}
	}

	// The Load hook does three jobs, all keyed off the filename:
	//
	//   Main.lua       (#1 of the wave) - the game has just cleared and is
	//                  reloading everything, so the lua_manager is rebuilt
	//                  against the state. Matches what Hell2Modding does.
	//   every script   - fire rom.on_import.pre / .post around it, which is
	//                  the API ModUtil 4.0.1 needs to inject itself.
	//   RoomManager.lua (#100, last) - not used yet; this is where mods that
	//                  want ModImporter's insertion point will go.
	//
	// HAS_CRASHED is snapshotted and restored around our callbacks. It is a
	// sticky global: if a mod's callback trips it, every later Load returns
	// false and the player gets a Supergiant "corrupt data file" dialog.
	bool load_detour(const char* path)
	{
		// Main.lua is #1 of every wave, so this rebuilds once per wave - the
		// clear/reload cycle the game does at boot and again on save load.
		//
		// It must be here and not at first Update: Update only begins ticking
		// once gameplay starts, 67ms after the last import of the wave, so a
		// manager built there has already missed every on_import it exists to
		// see. Initialising here panicked before the three static-address fixes
		// went in; that was the ABI, not the timing.
		if (path && !strcmp(path, "Main.lua"))
		{
			big::hades1::init_lua_manager_for_game_state();
		}

		const auto has_crashed = reinterpret_cast<bool*>(big::hades1::g_symbols.has_crashed);
		const bool crashed_before = has_crashed ? *has_crashed : false;

		big::lua_manager_extension::fire_on_pre_import(path);

		const bool result = g_load_hook.call<bool, const char*>(path);

		big::lua_manager_extension::fire_on_post_import(path);

		LOGF(DEBUG, "Load(\"{}\") -> {}", path ? path : "(null)", result);

		if (has_crashed && *has_crashed && !crashed_before)
		{
			// The game itself may legitimately set this; only clear it when we
			// are confident it was not set before we started meddling.
			LOGF(WARNING, "HAS_CRASHED was set during Load(\"{}\"); restoring it.", path ? path : "(null)");
			*has_crashed = false;
		}

		return result;
	}

	// ScriptManager::Load compiles a script with luaL_loadbufferx and then
	// pcalls it. This is the moment the compiled chunk is on top of the stack,
	// so it is the only point where an _ENV from on_import.pre can be attached
	// to it. Staging without this hook does nothing at all.
	//
	// The engine pcalls constantly, so apply_staged_env early-outs when nothing
	// is staged - which is almost always.
	int lua_pcallk_detour(lua_State* L, int nargs, int nresults, int errfunc, int ctx, void* k)
	{
		big::lua_manager_extension::apply_staged_env(L);
		return g_lua_pcallk_hook.call<int, lua_State*, int, int, int, int, void*>(L, nargs, nresults, errfunc, ctx, k);
	}

	// ScriptManager::InitLua builds the engine's Lua state. Some engine config
	// switches - EnableLuaMessageHook in particular - are only read while that
	// happens, so setting them afterwards is a no-op. Setting them here, before
	// the original runs, is the only point that works, and it re-applies on
	// every wave for free.
	//
	// InitLua is static (SA) and takes nothing.
	void init_lua_detour()
	{
		big::hades1::apply_pre_init_lua_config_options();
		g_init_lua_hook.call<void>();
	}

	// The game closes its lua_State between load waves and once more on quit.
	// Tear the manager down here, while its registry refs are still valid -
	// doing it later (or letting static destruction do it at exit) unrefs
	// against freed memory and takes the process out with an access violation.
	// That was the "fatal error" popup when quitting from the main menu.
	void lua_close_detour(lua_State* L)
	{
		big::hades1::shutdown_lua_manager(L);
		g_lua_close_hook.call<void, lua_State*>(L);
	}

	void update_detour(const float* dt)
	{
		static bool first_call = true;
		if (first_call)
		{
			first_call = false;
			LOGF(INFO, "First ScriptManager::Update on thread {}. Lua is live from here on.", GetCurrentThreadId());

			// Fallback only. If a wave somehow went by without Main.lua we
			// still come up, rather than leaving mods unloaded entirely.
			if (!big::hades1::is_lua_state_valid())
			{
				LOG(WARNING) << "No Main.lua seen; initialising late, on_import will have been missed.";
				big::hades1::init_lua_manager_for_game_state();
			}
		}

		// Original first, so the engine has finished its own per-frame script
		// work before anything of ours runs.
		g_update_hook.call<void, const float*>(dt);

		run_pending_tasks();
	}
} // namespace

namespace big::hades1
{
	bool install_script_hook()
	{
		if (!g_symbols.script_manager_update)
		{
			LOG(ERROR) << "ScriptManager::Update unresolved; not hooking.";
			return false;
		}

		g_update_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_symbols.script_manager_update),
		                                          reinterpret_cast<void*>(&update_detour));

		if (!g_update_hook)
		{
			LOG(ERROR) << "safetyhook refused ScriptManager::Update.";
			return false;
		}

		if (g_symbols.script_manager_load)
		{
			g_load_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_symbols.script_manager_load),
			                                        reinterpret_cast<void*>(&load_detour));
			LOG(INFO) << (g_load_hook ? "Hooked ScriptManager::Load." : "safetyhook refused ScriptManager::Load.");
		}

		if (const auto pcallk_addr = big::hades1::game_symbol("lua_pcallk"))
		{
			g_lua_pcallk_hook = safetyhook::create_inline(reinterpret_cast<void*>(pcallk_addr),
			                                              reinterpret_cast<void*>(&lua_pcallk_detour));
			LOG(INFO) << (g_lua_pcallk_hook ? "Hooked lua_pcallk for _ENV injection." : "safetyhook refused lua_pcallk.");
		}
		else
		{
			LOG(WARNING) << "lua_pcallk not in the symbol map; on_import.pre _ENV injection will not work.";
		}

		if (g_symbols.script_manager_init_lua)
		{
			g_init_lua_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_symbols.script_manager_init_lua),
			                                            reinterpret_cast<void*>(&init_lua_detour));
			LOG(INFO) << (g_init_lua_hook ? "Hooked ScriptManager::InitLua." : "safetyhook refused ScriptManager::InitLua.");
		}

		if (const auto lua_close_addr = big::hades1::game_symbol("lua_close"))
		{
			g_lua_close_hook = safetyhook::create_inline(reinterpret_cast<void*>(lua_close_addr),
			                                             reinterpret_cast<void*>(&lua_close_detour));
			LOG(INFO) << (g_lua_close_hook ? "Hooked lua_close." : "safetyhook refused lua_close.");
		}
		else
		{
			LOG(WARNING) << "lua_close not in the symbol map; manager teardown will be unsafe.";
		}

		LOG(INFO) << "Hooked ScriptManager::Update.";
		return true;
	}

	void run_on_script_thread(std::function<void()> task)
	{
		std::lock_guard lock(g_task_mutex);
		g_tasks.emplace_back(std::move(task));
	}
} // namespace big::hades1
