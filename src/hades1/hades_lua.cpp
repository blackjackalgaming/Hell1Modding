#include "common.hpp"

#include "file_manager/file_manager.hpp"
#include "game_pdb.hpp"
#include "hades_lua.hpp"
#include "lua/lua_manager.hpp"
#include "lua_extensions/lua_manager_extension.hpp"
#include "lua_extensions/lua_module_ext.hpp"
#include "pdb_symbol_map.hpp"
#include "version.hpp"

extern "C"
{
#include <lobject.h>
#include <ltable.h>
#include <lstate.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace
{
	// Manager lifetime state lives in lua_manager_extension - this file drives
	// it, but the API half needs it too.
} // namespace

namespace big::hades1
{
	// luaO_nilobject is a file-static const TValue in lobject.c: no PDB symbol,
	// and no public function returns its address. Recovered by decoding the
	// RIP-relative lea in lua_type (RVA 0x3792A0), which loads it to compare
	// against index2addr's result:
	//
	//   48 8D 0D 98 E0 12 00   lea rcx, [rip+0x12E098]
	//   48 3B C1               cmp rax, rcx
	//
	//   lea at 0x3792A9, 7 bytes long, so RIP = 0x3792B0
	//   0x3792B0 + 0x12E098 = 0x4A7348
	//
	// Verified: lands in .rdata, and the 16 bytes there are all zero, i.e.
	// value_ = 0 and tt_ = 0 = LUA_TNIL. Checked at runtime below.
	constexpr uintptr_t RVA_LUAO_NILOBJECT = 0x4A7348;

	// Point our Lua copy's sentinels at the game's.
	//
	// luaO_nilobject and dummynode are single static addresses that Lua
	// compares pointers against - "this table slot is absent", "this table has
	// no hash part". Statically linking a second Lua gives that copy its own
	// pair, so the two disagree and the VM corrupts. lua-fork-hades2 turns both
	// into volatile externs, defaulting to 0, for us to fill in here.
	//
	// Nothing in our Lua may run before this succeeds: at 0, luaO_nilobject is
	// a null pointer, which is worse than the mismatch it fixes.
	//
	// Hell2Modding gets both by calling index2addr and luaH_new out of the
	// game. Neither is available to us: Hades 1 links a prebuilt Lua, so the
	// PDB carries only the public API (lua_*, luaL_*) and not LUAI_FUNC
	// internals or file statics. dummynode is still reachable through the
	// public API; luaO_nilobject is not, and comes from the constant below.
	template<typename T>
	T game_func(const char* name)
	{
		const auto it = hades1_symbol_to_address.find(name);
		if (it == hades1_symbol_to_address.end())
		{
			return nullptr;
		}
		return reinterpret_cast<T>(static_cast<uintptr_t>(it->second));
	}

	bool point_sentinels_at_game_lua(lua_State* game_state)
	{
		const auto engine_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("EngineWin64s.dll"));

		// A table built with no hash part has node == dummynode. lua_topointer
		// hands back the Table* for a table value, so the whole derivation is
		// public API and survives a game update without any hardcoding.
		const auto game_lua_createtable = game_func<void (*)(lua_State*, int, int)>("lua_createtable");
		const auto game_lua_topointer   = game_func<const void* (*)(lua_State*, int)>("lua_topointer");
		const auto game_lua_settop      = game_func<void (*)(lua_State*, int)>("lua_settop");

		if (!game_lua_createtable || !game_lua_topointer || !game_lua_settop)
		{
			LOG(ERROR) << "Missing lua_createtable/lua_topointer/lua_settop in the symbol map.";
			return false;
		}

		game_lua_createtable(game_state, 0, 0);
		const auto* game_table = static_cast<const Table*>(game_lua_topointer(game_state, -1));
		if (!game_table)
		{
			LOG(ERROR) << "lua_topointer returned null for a fresh table.";
			game_lua_settop(game_state, -2);
			return false;
		}
		dummynode_external_address = reinterpret_cast<intptr_t>(game_table->node);
		game_lua_settop(game_state, -2);

		// luaO_nilobject is a file-static const TValue - no symbol, and no
		// public function returns its address. Read out of Ghidra instead, and
		// checked at runtime the same way the other RVAs are.
		if (!RVA_LUAO_NILOBJECT)
		{
			LOG(ERROR) << "RVA_LUAO_NILOBJECT is not set; see 'The Lua ABI problem' in CLAUDE.md.";
			return false;
		}
		luaO_nilobject_external_address = static_cast<intptr_t>(engine_base + RVA_LUAO_NILOBJECT);

		// Cheap sanity check that the hardcoded RVA still points at a nil TValue.
		// If a game update moves it, this catches the mistake before the VM does.
		const auto* candidate = reinterpret_cast<const TValue*>(luaO_nilobject_external_address);
		if (candidate->tt_ != LUA_TNIL)
		{
			LOGF(ERROR,
			     "RVA_LUAO_NILOBJECT ({:#X}) does not point at a nil TValue (tt_={}); game updated?",
			     RVA_LUAO_NILOBJECT,
			     candidate->tt_);
			return false;
		}

		if (!luaO_nilobject_external_address || !dummynode_external_address)
		{
			LOGF(ERROR,
			     "Sentinel lookup produced a null: nilobject {:#X}, dummynode {:#X}",
			     static_cast<uintptr_t>(luaO_nilobject_external_address),
			     static_cast<uintptr_t>(dummynode_external_address));
			return false;
		}

		// Third static-address mismatch, and the one the fork does not cover.
		// luaL_setfuncs opens with luaL_checkversion, which compares
		// G(L)->version - stamped by the game's Lua with the address of *its*
		// static `version` (lstate.c:290) - against our lua_version(NULL),
		// which returns *ours*. Two statically linked copies never agree, so
		// every sol2 call that binds functions died on "multiple Lua VMs
		// detected" (lauxlib.c:947).
		//
		// Repoint the state at our copy's static. The game only calls
		// luaL_checkversion from luaL_openlibs / luaL_requiref during InitLua,
		// which has long finished by the time we run.
		G(game_state)->version = lua_version(nullptr);

		LOGF(DEBUG,
		     "Lua sentinels borrowed from the game: nilobject RVA {:#X}, dummynode RVA {:#X}",
		     static_cast<uintptr_t>(luaO_nilobject_external_address) - engine_base,
		     static_cast<uintptr_t>(dummynode_external_address) - engine_base);
		return true;
	}

	uintptr_t game_symbol(const char* name)
	{
		const auto it = hades1_symbol_to_address.find(name);
		return it == hades1_symbol_to_address.end() ? 0 : static_cast<uintptr_t>(it->second);
	}

	bool is_lua_state_valid()
	{
		return lua_manager_extension::is_lua_state_valid();
	}

	// The game closes and recreates its lua_State between load waves, and once
	// more on quit. Either way the manager holds registry refs into the state
	// that is going away, and ~lua_module unrefs them - so this has to run
	// while the state is still alive.
	//
	// Without it the refs are unref-ed later against freed memory:
	// EXCEPTION_ACCESS_VIOLATION inside lua_rawgeti, either on the next wave or
	// during static destruction at exit. The latter is the "fatal error" popup
	// on quitting from the main menu, and it logs nothing because the logger is
	// already gone by then.
	//
	// This is the same idea as Hell2Modding's the_state_is_going_down, driven
	// off the lua_close hook rather than a __gc metamethod.
	void shutdown_lua_manager(lua_State* closing_state)
	{
		using namespace lua_manager_extension;

		std::scoped_lock lock(g_manager_mutex);

		if (!g_lua_manager_instance || (closing_state && closing_state != g_last_state))
		{
			return;
		}

		LOGF(DEBUG, "Lua state {} is closing; tearing the manager down.", static_cast<void*>(g_last_state));

		g_is_lua_state_valid = false;
		clear_staged_env();
		g_lua_manager_instance.reset();
		g_last_state = nullptr;
	}

	void init_lua_manager_for_game_state()
	{
		using namespace lua_manager_extension;

		std::scoped_lock lock(g_manager_mutex);

		if (!g_symbols.lua_interface)
		{
			LOG(ERROR) << "LUA_INTERFACE unresolved; cannot reach the game's Lua state.";
			return;
		}

		// LUA_INTERFACE.state sits at offset 0.
		lua_State* game_state = *reinterpret_cast<lua_State**>(g_symbols.lua_interface);
		if (!game_state)
		{
			LOG(ERROR) << "LUA_INTERFACE.state is null; the VM is not up yet.";
			return;
		}

		if (!point_sentinels_at_game_lua(game_state))
		{
			return;
		}

		// Normally the lua_close hook has already torn the old manager down by
		// the time a new wave starts, so this branch does not run. It is the
		// fallback for a state being abandoned without being closed: the old
		// manager's registry refs point into freed memory and ~lua_module would
		// unref them, so it is deliberately leaked instead.
		if (g_lua_manager_instance)
		{
			LOGF(INFO,
			     "Rebuilding without a preceding lua_close; leaking the previous manager (state {} -> {}).",
			     static_cast<void*>(g_last_state),
			     static_cast<void*>(game_state));
			(void)g_lua_manager_instance.release();
		}

		g_is_lua_state_valid = false;
		g_last_state         = game_state;

		g_lua_manager_instance = std::make_unique<lua_manager>(game_state,
		                                                      big::version::VERSION_NUMBER,
		                                                      g_file_manager.get_project_folder("config"),
		                                                      g_file_manager.get_project_folder("plugins_data"),
		                                                      g_file_manager.get_project_folder("plugins"),
		                                                      init_lua_api,
		                                                      make_module_env);

		// false: no file watcher. Hot reload needs the LOADED_SCRIPT_FILES
		// erase path that CLAUDE.md files under v2.
		g_lua_manager_instance->init<lua_module_ext>(false);

		g_is_lua_state_valid = true;

		LOGF(DEBUG,
		     "Lua manager up on the game's state {}; {} module(s) loaded.",
		     static_cast<void*>(game_state),
		     g_lua_manager->get_module_count());
	}
} // namespace big::hades1
