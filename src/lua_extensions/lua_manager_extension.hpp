#pragma once

#include <lua/lua_manager.hpp>

// Everything a mod sees, and nothing about how we reach the game.
//
// The split mirrors Hell2Modding: hades1/hades_lua.cpp acquires the game's
// lua_State and fixes the ABI sentinels, then hands it here to have the API
// built on top. Keeping the seam in the same place they put it means their
// bindings port straight across into this file rather than piling up in the
// game-specific one.
namespace big::lua_manager_extension
{
	// Shared with hades1/hades_lua.cpp, which owns the lifecycle. The mutex is
	// recursive because is_lua_state_valid() is called from inside functions
	// that already hold it.
	inline std::recursive_mutex g_manager_mutex;
	inline bool g_is_lua_state_valid = false;
	inline std::unique_ptr<lua_manager> g_lua_manager_instance;

	// The lua_State the live manager was built against, for the rebuild and
	// teardown checks.
	inline lua_State* g_last_state = nullptr;

	// Passed to lua_manager as its on_lua_state_init callback. Opens any
	// standard library the game left out, registers rom.on_import, and binds
	// lpeg / luasocket / paths.
	void init_lua_api(sol::state_view& state, sol::table& lua_ext);

	// Passed to lua_manager as its get_env_for_module callback: builds the
	// per-plugin _ENV from a copy of the game's _G.
	sol::environment make_module_env(sol::state_view& state);

	// Fired from the ScriptManager::Load hook. `pre` may return an _ENV to
	// install for the script about to load, which is how ModUtil injects
	// itself into game scripts.
	void fire_on_pre_import(const char* script_file);
	void fire_on_post_import(const char* script_file);

	// Applies an _ENV staged by on_import.pre to the chunk sitting on top of
	// the Lua stack, immediately before the engine pcalls it. Called from the
	// lua_pcallk hook - staging alone does nothing without this.
	//
	// Cheap no-op when nothing is staged, which is nearly every call: the
	// engine pcalls constantly.
	void apply_staged_env(lua_State* L);

	// Drops any _ENV staged by an on_import.pre callback. Called on teardown.
	void clear_staged_env();

	// True once the manager exists and the game's state is usable.
	bool is_lua_state_valid();
} // namespace big::lua_manager_extension
