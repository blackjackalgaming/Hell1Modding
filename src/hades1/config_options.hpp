#pragma once

namespace big::hades1
{
	// sgg::ConfigOptions is a set of public static bools the engine reads at
	// runtime - the game's own switches, not something we have to hook. Writing
	// them is far safer than detouring sgg::HandleException or
	// _FailedAssertHades, whose signatures and enum values we would be guessing
	// at. See "Engine config switches" in CLAUDE.md.
	//
	// Binds the config entries. Call once, after config::init_general().
	void init_config_option_entries();

	// Applies everything that can be set at any time (bug reporter, asserts).
	// Call after resolve_known_symbols().
	void apply_config_options();

	// Applies the options that must be set *before* the engine builds its Lua
	// state. Called from the ScriptManager::InitLua hook.
	void apply_pre_init_lua_config_options();
} // namespace big::hades1
