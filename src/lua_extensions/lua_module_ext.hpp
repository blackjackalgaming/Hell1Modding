#pragma once

#include "lua/lua_module.hpp"

namespace big
{
	// Per-mod callback lists for the rom.on_import API. This is not in
	// RoMBase - Hell2Modding adds it, and ModUtil 4.0.1 requires it, so
	// Hell1Modding has to provide the same surface.
	//
	// Mirrors hell2-reference/src/lua_extensions/lua_module_ext.hpp, minus the
	// Hades-2-only members (sjson data paths, keybinds, button hover).
	struct lua_module_data_ext
	{
		std::vector<sol::protected_function> m_on_pre_import;
		std::vector<sol::protected_function> m_on_post_import;
	};

	class lua_module_ext : public lua_module
	{
	public:
		lua_module_data_ext m_data_ext;

		lua_module_ext(const module_info& module_info, sol::environment& env) :
		    lua_module(module_info, env)
		{
		}

		lua_module_ext(const module_info& module_info, sol::state_view& state) :
		    lua_module(module_info, state)
		{
		}

		inline void cleanup() override
		{
			lua_module::cleanup();
			m_data_ext = {};
		}
	};
} // namespace big
