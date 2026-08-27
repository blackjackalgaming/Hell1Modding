#include "common.hpp"

#include "lua_extensions/lua_manager_extension.hpp"
#include "save_ignores.hpp"

// Hades 1 saves by **blacklist**, and that is the whole problem.
//
// Content/Scripts/Main.lua:1018:
//
//     function Save()
//         local saveTable = {}
//         for key, value in pairs( _G ) do
//             if value ~= nil and not SaveIgnores[key] then
//                 local valueType = type(value)
//                 if valueType ~= "function" and valueType ~= "userdata" and valueType ~= "thread" then
//                     saveTable[key] = value
//                 end
//             end
//         end
//         _saveData = assert( luabins.save( saveTable ) )
//     end
//
// Every global that is not explicitly ignored gets serialised, and the type
// filter is only applied at the top level - luabins then recurses into each
// surviving table. So one table in _G holding a function anywhere inside it
// fails the whole save.
//
// ReturnOfModdingBase publishes its API as a global named "rom". That is
// harmless in Hades 2, which saves by **whitelist** (GlobalSaveWhitelist), and
// fatal here: rom.path.*, rom.toml.* and the rest are sol2 functions and
// usertypes. Measured on a live install, this produced
//
//     Script Crash: Main.lua:1035 unsupported type detected
//
// on the first room transition, and once the game's own ValidateTypes was
// switched on,
//
//     sol: cannot call '__pairs' on type 'sol::as_container_t<TOMLDate>'
//
// Supergiant already ignore package, io, os, debug, coroutine, table, string,
// math and bit32 for exactly this reason, so adding to that list is the
// game's own designed mechanism rather than a workaround.
//
// This is deliberately not a Lua-side fix in a plugin: it has to hold for
// every user with no mod installed to provide it, and the loader is the thing
// that created the global.

namespace big::hades1
{
	namespace
	{
		// Globals the loader itself introduces into the game's _G.
		//
		// `require` is a function, so Save()'s top-level filter already drops
		// it, and package/io/os are in the game's own SaveIgnores. `rom` is
		// the only one that gets through.
		constexpr const char* loader_globals[] = {
		    "rom",
		};
	}

	void register_loader_save_ignores()
	{
		std::scoped_lock lock(lua_manager_extension::g_manager_mutex);

		if (!lua_manager_extension::is_lua_state_valid())
		{
			return;
		}

		sol::state_view state(lua_manager_extension::g_last_state);

		// Defined by Main.lua, so this is nil until that script has run.
		// Calling before then is a no-op rather than an error: the next wave
		// gets another chance, and a failure here only costs saving.
		sol::optional<sol::table> save_ignores = state["SaveIgnores"];
		if (!save_ignores)
		{
			LOG(WARNING) << "SaveIgnores is not defined yet; the game's save will try to serialise the loader's globals.";
			return;
		}

		for (const auto* name : loader_globals)
		{
			if (!state[name].valid())
			{
				continue;
			}

			save_ignores.value()[name] = true;
			LOGF(DEBUG, "Excluded loader global \"{}\" from the game's save.", name);
		}
	}
}
