#include "common.hpp"

#include "paths_ext.hpp"

// Ported from hell2-reference/src/lua_extensions/bindings/paths_ext.cpp.
//
// Content maps cleanly: the exe sits in <game>/x64, so its parent + "Content"
// is <game>/Content, which is where Scripts/, Game/, Maps/ etc. live. Verified
// against the real install.
//
// **Ship does not.** Hades 1 has no folder of that name - the layout is just
// Content/, x64/, x64Vk/, x86/. It is kept because it is part of the API a
// Hades 2 mod expects, and returning the executable folder (x64/) is the
// closest analogue, but nothing in Hades 1 is actually called "Ship".
//
// rom.path.combine is already in RoMBase (lua/bindings/path.cpp), so only
// these two were missing.

namespace lua::paths_ext
{
	std::filesystem::path get_game_executable_folder()
	{
		constexpr size_t max_path = MAX_PATH * 2;
		wchar_t buffer[max_path];
		DWORD length = GetModuleFileNameW(nullptr, buffer, max_path);
		if (length == 0 || length == max_path)
		{
			return {};
		}

		std::filesystem::path exe_path(buffer);
		return exe_path.parent_path();
	}

	// Lua API: Function
	// Table: paths
	// Name: Content
	// The game's Content folder, which holds Scripts, Game, Maps, Audio and so on.
	// Returns: string: absolute path to <game>/Content
	static std::string hades_Content()
	{
		auto folder  = get_game_executable_folder().parent_path();
		folder      /= "Content";

		return (char*)folder.u8string().c_str();
	}

	// Lua API: Function
	// Table: paths
	// Name: Ship
	// The folder the executable lives in. Note Hades 1 has no folder actually
	// named "Ship" - the layout is Content/, x64/, x64Vk/, x86/ - so this
	// returns x64/. The name is kept for parity with Hades II.
	// Returns: string: absolute path to <game>/x64
	static std::string hades_Ship()
	{
		return (char*)get_game_executable_folder().u8string().c_str();
	}

	void bind(sol::table& state)
	{
		// get_or_create rather than create_named: RoMBase already made this
		// table for config/plugins_data/plugins, and clobbering it would drop
		// those.
		auto ns = state["paths"].get_or_create<sol::table>();

		ns["Content"] = hades_Content;
		ns["Ship"]    = hades_Ship;
	}
} // namespace lua::paths_ext
