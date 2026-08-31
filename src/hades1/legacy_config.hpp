#pragma once

namespace big::hades1
{
	// Give a legacy mod's config.lua the same treatment Chalk gives a plugin's:
	// a config/<Mod>.cfg the overlay can edit, with the .cfg as the source of
	// truth. Call immediately after the mod's config.lua has been imported.
	void bind_legacy_config(const std::string& mod_name);

	// Remember which globals exist, so the next import's additions can be
	// spotted. Call immediately before importing a config.lua.
	void snapshot_globals();

	// One entry per legacy mod that had a config.lua, in name order.
	//
	// The overlay's Config tab walks lua_module::m_data.m_config_files, which
	// only ever holds files a *plugin* made through the config.config_file Lua
	// API. A Content/Mods mod is not a lua_module and cannot call anything, so
	// without this its .cfg is written and then invisible.
	struct legacy_config_file
	{
		std::string mod_name;
		std::unique_ptr<toml_v2::config_file> file;
	};

	const std::vector<legacy_config_file>& legacy_config_files();
}
