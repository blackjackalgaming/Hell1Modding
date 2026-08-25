#include "common.hpp"

#include "config/config.hpp"
#include "config_options.hpp"
#include "hades_lua.hpp"

namespace
{
	toml_v2::config_file::config_entry<bool>* g_lua_message_hook   = nullptr;
	toml_v2::config_file::config_entry<bool>* g_suppress_reporter  = nullptr;
	toml_v2::config_file::config_entry<bool>* g_suppress_asserts   = nullptr;

	// Every one of these is `?<name>@ConfigOptions@sgg@@2_NA` - a public static
	// bool - so this is a one-byte write, not a hook.
	bool set_flag(const char* symbol, bool value)
	{
		const auto address = big::hades1::game_symbol(symbol);
		if (!address)
		{
			LOGF(WARNING, "{} not in the symbol map; leaving it alone.", symbol);
			return false;
		}

		auto* flag = reinterpret_cast<bool*>(address);
		if (*flag == value)
		{
			LOGF(DEBUG, "{} already {}.", symbol, value);
			return true;
		}

		LOGF(INFO, "{}: {} -> {}", symbol, *flag, value);
		*flag = value;
		return true;
	}
} // namespace

namespace big::hades1
{
	void init_config_option_entries()
	{
		g_lua_message_hook = config::general->bind(
		    "Engine",
		    "Lua Message Hook",
		    true,
		    "Sets sgg::ConfigOptions::EnableLuaMessageHook before the engine builds its Lua state, which installs the "
		    "engine's l_msghandler/l_panic. Turns silent Lua failures into real tracebacks in the log. Costs nothing "
		    "when no error occurs.");

		g_suppress_reporter = config::general->bind(
		    "Engine",
		    "Suppress Supergiant Bug Reporter",
		    true,
		    "Turns off the game's internal bug reporter and automatic assert reporting. On a modded install a crash is "
		    "far more likely to be a mod than a game bug, and reporting it to Supergiant helps nobody. Hell1Modding's "
		    "own exception handler still writes a dump.");

		g_suppress_asserts = config::general->bind(
		    "Engine",
		    "Suppress Script Error Asserts",
		    false,
		    "Turns off sgg::ConfigOptions::ScriptErrorAsserts, so a Lua error does not assert the game down. Off by "
		    "default: it keeps the game running past a fault, which can mask a real problem and leave the engine in a "
		    "questionable state. Useful while developing a mod.");
	}

	void apply_config_options()
	{
		if (g_suppress_reporter && g_suppress_reporter->get_value())
		{
			set_flag("sgg::ConfigOptions::UseInternalBugReporter", false);
			set_flag("sgg::ConfigOptions::AutoReportAsserts", false);
		}

		if (g_suppress_asserts && g_suppress_asserts->get_value())
		{
			set_flag("sgg::ConfigOptions::ScriptErrorAsserts", false);
			set_flag("sgg::ConfigOptions::AssetErrorAsserts", false);
		}
	}

	void apply_pre_init_lua_config_options()
	{
		// Must happen before ScriptManager::InitLua builds the state - the
		// engine only reads this while installing its message handler, so
		// setting it afterwards does nothing. Hence the InitLua hook.
		if (g_lua_message_hook && g_lua_message_hook->get_value())
		{
			set_flag("sgg::ConfigOptions::EnableLuaMessageHook", true);
		}
	}
} // namespace big::hades1
