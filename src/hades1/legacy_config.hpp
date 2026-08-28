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
}
