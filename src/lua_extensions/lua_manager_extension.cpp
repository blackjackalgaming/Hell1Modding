#include "common.hpp"

#include "bindings/lpeg.hpp"
#include "bindings/luasocket/luasocket.hpp"
#include "bindings/hades/audio.hpp"
#include "bindings/paths_ext.hpp"
#include "bindings/tolk/tolk.hpp"
#include "lua_manager_extension.hpp"
#include "lua_module_ext.hpp"

extern "C"
{
#include <lauxlib.h>
#include <lualib.h>
}

namespace
{
	// Set by an on_import.pre callback that wants to own the _ENV of the script
	// currently being loaded. Consumed by the Load hook, cleared on post.
	sol::optional<sol::environment> g_env_to_add;
} // namespace

namespace big::lua_manager_extension
{
	bool is_lua_state_valid()
	{
		std::scoped_lock lock(g_manager_mutex);
		return g_is_lua_state_valid && g_lua_manager;
	}

	void clear_staged_env()
	{
		std::scoped_lock lock(g_manager_mutex);
		g_env_to_add.reset();
	}

	// ScriptManager::Load compiles with luaL_loadbufferx and then pcalls the
	// resulting chunk. Between those two steps the chunk is the value on top of
	// the stack, so this is the one moment an _ENV can be attached to it -
	// which is why on_import.pre stages and this applies.
	//
	// This is the half that was missing: without it, a callback returning an
	// _ENV had no effect whatsoever, and ModUtil's whole injection mechanism
	// silently did nothing.
	void apply_staged_env(lua_State* L)
	{
		// The engine pcalls constantly, so stay cheap when idle. Reading the
		// optional under the lock is the whole cost.
		std::scoped_lock lock(g_manager_mutex);

		if (!g_env_to_add.has_value() || !g_env_to_add.value().valid())
		{
			return;
		}

		// **Consume it.** An environment is staged for exactly one script and
		// belongs to exactly one compile - the first luaL_loadbufferx after
		// on_import.pre, which is the script Load was called for.
		//
		// Leaving it staged leaked it into nested imports. A Hades 1 script
		// Imports others, and each Import is a recursive ScriptManager::Load
		// inside the outer one, so RoomManager.lua's environment was also
		// attached to Color.lua - the first script it imports - and that
		// nested import's fire_on_post_import then cleared the staging, so
		// every script after it silently got none. Both halves wrong.
		const sol::environment env = std::move(g_env_to_add.value());
		g_env_to_add.reset();

		sol::set_environment(env, sol::stack_reference(L, -1));
	}

	void init_lua_api(sol::state_view& state, sol::table& lua_ext)
	{
		// Lua API: Table
		// Name: on_import
		// Callbacks fired around each script the game imports from the game's
		// Content/Scripts folder. Required by ModUtil 4.0.1.
		auto on_import_table = lua_ext.create_named("on_import");

		// Lua API: Function
		// Table: on_import
		// Name: pre
		// Param: callback: function: signature (string script_name, current_ENV) returning nil or an _ENV
		// Called before the game loads a script from Content/Scripts. Returning a
		// table installs it as that script's _ENV, which is how ModUtil injects
		// itself. Chain to rom.game via __index/__newindex or the script's globals
		// will not reach the game.
		on_import_table.set_function("pre",
		                             [](sol::protected_function f, sol::this_environment env)
		                             {
			                             if (auto mod = (lua_module_ext*)lua_module::this_from(env))
			                             {
				                             mod->m_data_ext.m_on_pre_import.push_back(f);
			                             }
		                             });

		// Lua API: Function
		// Table: on_import
		// Name: post
		// Param: callback: function: signature (string script_name)
		// Called after the game has loaded a script from Content/Scripts. Note the
		// game loads its scripts twice at startup, so this fires once per wave.
		on_import_table.set_function("post",
		                             [](sol::protected_function f, sol::this_environment env)
		                             {
			                             if (auto mod = (lua_module_ext*)lua_module::this_from(env))
			                             {
				                             mod->m_data_ext.m_on_post_import.push_back(f);
			                             }
		                             });

		// Hades 1 ships a Lua state with several standard libraries left
		// unopened - `package`, `require`, `io` and `os` are all nil, confirmed
		// by probing the live state. Hades 2 has them and mods assume them:
		// SGG_Modding-SJSON opens with require("lpeg"), and
		// SGG_Modding-DemonDaemon indexes `io`.
		//
		// Rather than discover these one game-launch at a time, open any
		// standard library the game left out. Only ever additive: a library the
		// game already opened is skipped, so nothing it relies on is replaced.
		//
		// Has to happen before the per-module environments are built, since
		// those are copies of _G.
		{
			const struct
			{
				const char* name;
				lua_CFunction opener;
			} standard_libs[] = {
			    {LUA_LOADLIBNAME, luaopen_package  },
			    {LUA_COLIBNAME,   luaopen_coroutine},
			    {LUA_TABLIBNAME,  luaopen_table    },
			    {LUA_IOLIBNAME,   luaopen_io       },
			    {LUA_OSLIBNAME,   luaopen_os       },
			    {LUA_STRLIBNAME,  luaopen_string   },
			    {LUA_BITLIBNAME,  luaopen_bit32    },
			    {LUA_MATHLIBNAME, luaopen_math     },
			    {LUA_DBLIBNAME,   luaopen_debug    },
			};

			for (const auto& lib : standard_libs)
			{
				if (state[lib.name].valid())
				{
					continue;
				}

				luaL_requiref(state.lua_state(), lib.name, lib.opener, 1);
				lua_pop(state.lua_state(), 1);
				LOGF(DEBUG, "Opened missing standard library: {}", lib.name);
			}
		}

		// require("lpeg") - SGG_Modding-SJSON hard-requires it, and most of the
		// mod ecosystem depends on SJSON.
		lua::lpeg::bind(lua_ext);

		// socket / mime / http / ltn12. Nothing needs it today; it is here so a
		// mod written against Hell2Modding finds the same libraries.
		lua::luasocket::bind(lua_ext);

		// paths.Content / paths.Ship - SGG_Modding-SJSON needs Content on its
		// first line, and the whole ModUtil chain hangs off SJSON.
		lua::paths_ext::bind(lua_ext);

		// audio.load_bank - custom FMOD banks, via FMOD Studio's public API.
		lua::hades::audio::bind(lua_ext);

		// tolk.output / tolk.silence - screen reader output. The driver is not
		// loaded until a mod calls one of them.
		lua::tolk::bind(lua_ext);
	}

	sol::environment make_module_env(sol::state_view& state)
	{
		// rom.game = _G, so mods can reach the game's globals explicitly rather
		// than by accident.
		state[rom::g_lua_api_namespace]["game"] = state["_G"];

		sol::table plugin_G = state.create_table();
		sol::table all_g    = state["_G"];
		for (const auto& [k, v] : all_g)
		{
			plugin_G[k] = v;
		}

		plugin_G[rom::g_lua_api_namespace] = state[rom::g_lua_api_namespace];
		plugin_G["_G"]                     = plugin_G;

		return sol::environment(state, sol::create, plugin_G);
	}

	void fire_on_pre_import(const char* script_file)
	{
		std::scoped_lock lock(g_manager_mutex);

		if (!is_lua_state_valid() || !script_file)
		{
			return;
		}

		for (const auto& mod_ : g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext*)mod_.get();
			for (const auto& cb : mod->m_data_ext.m_on_pre_import)
			{
				auto res = cb(script_file, g_env_to_add.has_value() ? g_env_to_add.value() : sol::lua_nil);
				if (!res.valid())
				{
					LOG(ERROR) << mod->guid() << " on_import.pre threw: " << res.get<sol::error>().what();
					continue;
				}

				auto env_to_set = res.get<sol::optional<sol::environment>>();
				if (env_to_set && env_to_set.value().valid())
				{
					g_env_to_add = env_to_set;
					LOG(DEBUG) << "_ENV for " << script_file << " set by " << mod->guid();
				}
			}
		}
	}

	void fire_on_post_import(const char* script_file)
	{
		std::scoped_lock lock(g_manager_mutex);

		// Safety net only: apply_staged_env consumes the environment at the
		// compile it belongs to. This catches the case where no compile
		// happened at all - a script already in LOADED_SCRIPT_FILES returns
		// false from Load without ever being read.
		g_env_to_add.reset();

		if (!is_lua_state_valid() || !script_file)
		{
			return;
		}

		for (const auto& mod_ : g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext*)mod_.get();
			for (const auto& cb : mod->m_data_ext.m_on_post_import)
			{
				auto res = cb(script_file);
				if (!res.valid())
				{
					LOG(ERROR) << mod->guid() << " on_import.post threw: " << res.get<sol::error>().what();
				}
			}
		}
	}
} // namespace big::lua_manager_extension
