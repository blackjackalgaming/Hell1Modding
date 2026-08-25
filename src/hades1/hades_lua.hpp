#pragma once

// How we reach the game's Lua VM. What mods can *do* with it lives in
// lua_extensions/lua_manager_extension.hpp - the same seam Hell2Modding keeps
// between hades2/hades_lua.hpp and its lua_extensions, so their bindings port
// into that file rather than piling up in this one.

namespace big::hades1
{
	// Builds the lua_manager on top of the game's own lua_State and loads every
	// plugin in ReturnOfModding/plugins. Call once per load wave, on the script
	// thread, when Main.lua goes by.
	//
	// Mods are loaded through sol2 with absolute paths, not through
	// ScriptManager::Load - see roadmap item 4 in CLAUDE.md for why.
	void init_lua_manager_for_game_state();

	// Tears the manager down while the state it holds refs into is still alive.
	// Called from the lua_close hook - see the comment on the definition.
	void shutdown_lua_manager(lua_State* closing_state);

	// Absolute address of a symbol from EngineWin64s.pdb, or 0. Thin wrapper so
	// callers do not each reach into the map.
	uintptr_t game_symbol(const char* name);

	// True once the manager exists and the game's state is usable. Forwards to
	// lua_manager_extension so the hooks need only this header.
	bool is_lua_state_valid();

	// Requires lua-fork-hades2 plus point_sentinels_at_game_lua() having run;
	// see "The Lua ABI problem" in CLAUDE.md.
	inline bool g_enable_lua_manager = true;
} // namespace big::hades1
