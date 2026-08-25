#pragma once

namespace lua::paths_ext
{
	std::filesystem::path get_game_executable_folder();

	// Adds paths.Content and paths.Ship on top of RoMBase's paths table.
	// SGG_Modding-SJSON calls rom.paths.Content() on its first line, so the
	// whole ModUtil dependency chain fails without this.
	void bind(sol::table& state);
} // namespace lua::paths_ext
