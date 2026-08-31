#pragma once

namespace big::hades1
{
	// Register every engine-loadable asset under ReturnOfModding/plugins_data
	// so the engine opens the mod's copy instead of the one in Content.
	//
	// This is the plugin-format counterpart to what legacy Content/Mods mods
	// get through their modfile's Replace directive: both end up in the same
	// file_redirect table, driven by the same PlatformOpenFile hook.
	//
	// Call once, after g_file_manager.init and install_file_redirect_hook.
	void scan_plugin_data_files();

	// Read-only probe on fsGetFilesWithExtension, the engine's asset
	// enumeration. Logs what the engine asks for and how much it found, at
	// DEBUG. See the comment on the definition for why this does not yet
	// inject anything.
	bool install_asset_enumeration_probe();
}
