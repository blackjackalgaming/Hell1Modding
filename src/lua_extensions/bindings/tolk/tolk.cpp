#include "common.hpp"

#include "tolk.hpp"

#include <Tolk.h>

// Screen reader output, ported from
// hell2-reference/src/lua_extensions/bindings/tolk/tolk.cpp.
//
// **Only the game-agnostic half is here.** Theirs also exposes screen_read,
// get_lines_from_thing and on_button_hover, and all three walk Hades 2 engine
// structures - GUIComponentTextBox at a hardcoded offset 0x5A8, sgg::world,
// sgg::World::GetActiveThing - with static_asserts pinning the layout. Those
// are rewrites against Hades 1 symbols, not ports, exactly as recorded in
// CLAUDE.md for lua_extensions/bindings/hades/. Copying them would compile and
// then read whatever happens to live at those offsets in a different engine.
//
// What is here is pure Tolk API and behaves identically in either game.

namespace lua::tolk
{
	namespace
	{
		std::wstring utf8_to_wide(const std::string& utf8)
		{
			const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
			if (length <= 0)
			{
				return L"";
			}

			// length counts the terminator MultiByteToWideChar writes; drop it,
			// std::wstring carries its own.
			std::wstring wide(static_cast<size_t>(length) - 1, 0);
			MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);

			return wide;
		}

		std::string wide_to_utf8(const wchar_t* wide)
		{
			if (!wide)
			{
				return "";
			}

			const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
			if (length <= 0)
			{
				return "";
			}

			std::string utf8(static_cast<size_t>(length) - 1, 0);
			WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);

			return utf8;
		}

		// Tolk_Load initialises COM and pulls in the screen reader client DLLs,
		// so it is deferred until a mod actually asks for it rather than run at
		// every state init. Hell2Modding loads eagerly inside bind(); the Lua
		// side cannot tell the difference, and almost no Hades 1 install has a
		// screen reader mod to justify the work.
		bool ensure_loaded()
		{
			if (Tolk_IsLoaded())
			{
				return true;
			}

			// SAPI as a fallback driver, so output still works with no screen
			// reader installed. Nothing speaks until a mod calls output().
			Tolk_TrySAPI(true);
			Tolk_Load();

			if (!Tolk_IsLoaded())
			{
				LOG(WARNING) << "Tolk failed to load; screen reader output is unavailable.";
				return false;
			}

			const wchar_t* name = Tolk_DetectScreenReader();
			if (name)
			{
				LOG(INFO) << "The active screen reader driver is: " << wide_to_utf8(name);
			}
			else
			{
				LOG(INFO) << "None of the supported screen readers is running; falling back to SAPI.";
			}

			return true;
		}
	}

	// Lua API: Table
	// Name: tolk
	// Screen reader output, for mods that speak UI text aloud.
	//
	// The driver is loaded lazily on the first call. Hades 1 ships none of the
	// screen reader client DLLs, so NVDA / JAWS / System Access are only
	// detected if their own software is installed and running; otherwise Tolk
	// falls back to SAPI.

	// Lua API: Function
	// Table: tolk
	// Name: output
	// Param: str: string: The text to speak.
	// Returns: boolean: true if the text was handed to a driver.
	// Outputs text through the current screen reader driver.
	static bool output(const std::string& str)
	{
		if (!ensure_loaded())
		{
			return false;
		}

		return Tolk_Output(utf8_to_wide(str).c_str());
	}

	// Lua API: Function
	// Table: tolk
	// Name: silence
	// Returns: boolean: true if the driver acknowledged the request.
	// Stops whatever the screen reader is currently saying.
	static bool silence()
	{
		if (!ensure_loaded())
		{
			return false;
		}

		return Tolk_Silence();
	}

	// Lua API: Function
	// Table: tolk
	// Name: detect_screen_reader
	// Returns: string: The name of the active driver, or nil if none is running.
	// Reports which screen reader Tolk is talking to.
	static sol::object detect_screen_reader(sol::this_state state)
	{
		if (!ensure_loaded())
		{
			return sol::nil;
		}

		const wchar_t* name = Tolk_DetectScreenReader();
		if (!name)
		{
			return sol::nil;
		}

		return sol::make_object(state, wide_to_utf8(name));
	}

	// Lua API: Function
	// Table: tolk
	// Name: is_loaded
	// Returns: boolean: true once a driver has been loaded.
	// Whether the screen reader layer is up. Calling any other tolk function
	// loads it, so this is only false before the first use.
	static bool is_loaded()
	{
		return Tolk_IsLoaded();
	}

	void bind(sol::table& state)
	{
		auto ns = state.create_named("tolk");

		ns.set_function("output", output);
		ns.set_function("silence", silence);
		ns.set_function("detect_screen_reader", detect_screen_reader);
		ns.set_function("is_loaded", is_loaded);
	}
}
