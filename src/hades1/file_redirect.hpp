#pragma once

namespace big::hades1
{
	// Serve `source` whenever the engine opens `target`.
	//
	// `target` is Content-relative and as the modfile spells it, e.g.
	// "Win/Packages/OEHestia.pkg". Matching is case-insensitive and ignores
	// slash direction, because the engine and the modfile do not agree on
	// either.
	void add_file_redirect(const std::string& target, const std::filesystem::path& source, const std::string& owner);

	void clear_file_redirects();

	bool install_file_redirect_hook();
}
