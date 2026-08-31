#include "common.hpp"

#include "config/config.hpp"
#include "config_options.hpp"
#include "hades_lua.hpp"

namespace
{
	toml_v2::config_file::config_entry<bool>* g_lua_message_hook   = nullptr;
	toml_v2::config_file::config_entry<bool>* g_suppress_reporter  = nullptr;
	toml_v2::config_file::config_entry<bool>* g_suppress_asserts   = nullptr;
	toml_v2::config_file::config_entry<bool>* g_disable_analytics  = nullptr;
	toml_v2::config_file::config_entry<bool>* g_uncap_audio_pool   = nullptr;

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

	// `?<name>@ConfigOptions@sgg@@2HA` - a public static int. Same idea as
	// set_flag, four bytes instead of one.
	bool set_int(const char* symbol, int value)
	{
		const auto address = big::hades1::game_symbol(symbol);
		if (!address)
		{
			LOGF(WARNING, "{} not in the symbol map; leaving it alone.", symbol);
			return false;
		}

		auto* slot = reinterpret_cast<int*>(address);
		if (*slot == value)
		{
			LOGF(DEBUG, "{} already {}.", symbol, value);
			return true;
		}

		LOGF(INFO, "{}: {} -> {}", symbol, *slot, value);
		*slot = value;
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

		// Hell2Modding reaches the same outcome by detouring
		// sgg::PlatformAnalytics::Start to an empty function, and by hooking
		// sgg::registerField<bool> to force the UseAnalytics default false.
		// Neither is necessary here: Hades 1 exposes the flag itself as a
		// public static, so this is a one-byte write with nothing to go wrong.
		// (sgg::PlatformAnalytics::DISABLE exists too, but UseAnalytics is the
		// option the engine actually reads, and is the one the game's own
		// config file writes.)
		g_disable_analytics = config::general->bind(
		    "Engine",
		    "Disable Analytics",
		    true,
		    "Sets sgg::ConfigOptions::UseAnalytics false, so the engine does not start its GameAnalytics session. "
		    "Telemetry from a modded install is noise to Supergiant and traffic the player did not ask for.");

		g_uncap_audio_pool = config::general->bind(
		    "Engine",
		    "Uncap Audio Memory Pool",
		    false,
		    "Sets sgg::ConfigOptions::AudioMemoryPoolSize to 0, which Hell2Modding does on Hades 2 because mods adding "
		    "custom banks exhaust the fixed pool. UNTESTED on Hades 1 - the option exists here and has the same type, "
		    "but 0 has not been confirmed to mean 'no limit' in this engine build. Off by default; turn it on only if "
		    "custom audio is failing to load, and check the log for FMOD errors either way.");
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

		if (g_disable_analytics && g_disable_analytics->get_value())
		{
			set_flag("sgg::ConfigOptions::UseAnalytics", false);
		}

		if (g_uncap_audio_pool && g_uncap_audio_pool->get_value())
		{
			set_int("sgg::ConfigOptions::AudioMemoryPoolSize", 0);
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
