#pragma once

namespace big::hades1
{
	// Read Content/Mods/*/modfile.txt, the layout ModImporter has always used.
	// Called once per load wave, before the game's own scripts run.
	void scan_legacy_mods();

	// Run the imports a legacy mod declared against this script. `top` selects
	// `Top Import` (before the script runs) or `Import` (after it).
	void fire_legacy_imports(const char* script, bool top);

	// What the overlay needs to show them. These are not RoM modules, so they
	// are absent from g_lua_manager->m_modules and would otherwise be invisible
	// in the UI - someone with three mods in Content/Mods would see an empty
	// Mods tab and no way to tell whether any of them loaded.
	struct legacy_mod_info
	{
		std::string name;
		int priority      = 100;
		size_t imports    = 0;
		size_t missing    = 0; // imports whose file is not on disk
	};

	std::vector<legacy_mod_info> get_legacy_mods();
}
