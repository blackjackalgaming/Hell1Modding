#pragma once

namespace big::hades1
{
	// Tell Hades 1's save serialiser to skip the globals this loader adds.
	//
	// Call once per load wave, after Main.lua has run - that is where
	// SaveIgnores is defined (Content/Scripts/Main.lua:703).
	void register_loader_save_ignores();
}
