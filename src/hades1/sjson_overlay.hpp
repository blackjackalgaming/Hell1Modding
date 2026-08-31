#pragma once

namespace big::hades1::sjson_overlay
{
	// A mod drops replacement .sjson files in
	//   plugins_data/<mod-guid>/Hell1Modding-SJSON/<path under Content/Game>
	// and the engine loads those instead of its own.
	//
	// This is whole-file replacement keyed on the logical path, not the merge
	// that a legacy modfile's SJSON directive describes. Two mods editing
	// different fields of one file still conflict here; merging is a separate
	// job (see "SJSON is parsed but not applied" in CLAUDE.md).
	//
	// Keyed on the full Content-relative path rather than the bare filename,
	// because .sjson names repeat across directories - Hades 1 ships an
	// Enemies.sjson under Game/Units and unrelated files of the same shape
	// elsewhere, so leaf matching would be ambiguous.
	inline constexpr const char* DATA_DIR_NAME = "Hell1Modding-SJSON";

	// Index every mod's overlay directory. Call once at startup.
	void scan_all_plugin_data(const std::filesystem::path& plugins_data_path);

	// Absolute path for a Content-relative logical path such as
	// "Game/Animations/Fx.sjson", or empty when the mods do not override it.
	// `logical_relpath` must already be normalised.
	std::string lookup(const std::string& logical_relpath);

	// Normalised: forward slashes, no trailing separator.
	std::string normalize_path(std::string path);

	// Note that the engine has finished enumerating a directory, so a file
	// registered afterwards can be reported as too late to take effect.
	void mark_directory_enumerated(const std::string& normalized_subdir, const std::string& extension);

	// True once anything is registered - lets the hook skip the work entirely
	// on the overwhelmingly common no-SJSON-mods case.
	bool any_registered();
}
