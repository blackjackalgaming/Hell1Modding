#pragma once

namespace big::hades1
{
	// Physically install a legacy mod's .pkg / .pkg_manifest into the game's
	// own Content folder.
	//
	// This is the one place Hell1Modding writes into the game install, and it
	// exists because packages are not reliably reachable by interception alone:
	// the engine builds its package list by enumerating Content/Win/Packages at
	// boot, and a file that is not physically there is not in that list. Every
	// other asset kind is served from the mod's folder by file_redirect and
	// touches nothing.
	//
	// Everything installed is recorded, so a package belonging to a mod that
	// has since been removed is deleted again on the next launch. A file the
	// loader did not put there is never overwritten - see install_package.
	//
	// Call order per scan: begin, then install_package per file, then finish.
	void begin_package_install();

	// True when the file was installed (or is already installed and current),
	// meaning the caller should not also register a redirect for it. False
	// means "not handled here" - fall back to the redirect.
	bool install_package(const std::string& target, const std::filesystem::path& source, const std::string& owner);

	// Deletes anything installed by a previous run that is no longer wanted,
	// and writes the record back out.
	void finish_package_install();
}
