#pragma once

namespace big::gui
{
	// Registers the overlay's draw callback and its toggle-key config entry.
	// Call after config::init_general() and init_renderer().
	void init();

	bool is_open();
} // namespace big::gui
