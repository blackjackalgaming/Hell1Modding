#pragma once

namespace lua::hades::audio
{
	// Hooks FMOD::Studio::System::update to capture the engine's Studio System
	// pointer. Safe to call at any time - update runs every frame, so there is
	// no race against engine startup.
	bool install_hooks();

	void bind(sol::table& state);
} // namespace lua::hades::audio
