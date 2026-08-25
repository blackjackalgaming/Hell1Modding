#pragma once

namespace lua::lpeg
{
	// Registers the lpeg library so mods can `require("lpeg")`.
	//
	// Not optional for ecosystem parity: SGG_Modding-SJSON does a hard
	// `require("lpeg")`, and in a real 78-mod profile 50 mods depend on SJSON.
	// Without this, all of them fail to load.
	void bind(sol::table& state);
}
