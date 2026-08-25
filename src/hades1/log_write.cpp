#include "common.hpp"

#include "config/config.hpp"
#include "hades_lua.hpp"
#include "log_write.hpp"

#include <safetyhook.hpp>

namespace
{
	SafetyHookInline g_log_write_hook{};

	toml_v2::config_file::config_entry<bool>* g_output_vanilla_log = nullptr;

	// The engine's levels are bit flags, same lineage as Hades 2's.
	al::eLogLevel map_level(unsigned int level, const char*& out_label)
	{
		switch (level)
		{
		case 2:  out_label = "DBG";  return DEBUG;
		case 4:  out_label = "INFO"; return INFO;
		case 8:  out_label = "WARN"; return WARNING;
		case 16: out_label = "ERR";  return ERROR;
		default: out_label = "UNK";  return INFO;
		}
	}

	// Engine __FILE__ values are full build-machine paths, e.g.
	// d:\jenkins\workspace\minos_pc_latest\gsge\engine.native\code\script\scriptmanager.cpp
	// Hell2Modding trims a hardcoded 41 characters; taking the basename instead
	// survives a build-path change and reads the same.
	const char* basename_of(const char* path)
	{
		if (!path)
		{
			return "?";
		}

		const char* last = path;
		for (const char* p = path; *p; ++p)
		{
			if (*p == '\\' || *p == '/')
			{
				last = p + 1;
			}
		}
		return last;
	}

	void log_write_detour(unsigned int level, const char* filename, int line_number, const char* message, ...)
	{
		std::string formatted;
		if (message)
		{
			va_list args;
			va_start(args, message);
			const int size = vsnprintf(nullptr, 0, message, args);
			va_end(args);

			if (size > 0)
			{
				formatted.resize(static_cast<size_t>(size) + 1);
				va_start(args, message);
				vsnprintf(formatted.data(), formatted.size(), message, args);
				va_end(args);
				formatted.resize(static_cast<size_t>(size)); // drop the NUL
			}
		}

		// Pass the already-formatted text through as a "%s" argument rather
		// than as the format string. Hell2Modding re-passes it as the format,
		// which misinterprets any literal % the message happens to contain.
		g_log_write_hook.call<void, unsigned int, const char*, int, const char*, const char*>(level,
		                                                                                     filename,
		                                                                                     line_number,
		                                                                                     "%s",
		                                                                                     formatted.c_str());

		// The engine cannot flush its own log FileStream and says so once per
		// write - 577 times in a boot. It is not our doing: vanilla Hades with
		// the loader removed writes no log file either, so its file logging is
		// simply broken in this build and we are the first thing to surface it.
		// Report it once so the fact is not hidden, then drop the repeats.
		if (formatted.starts_with("Error flushing system FileStream"))
		{
			static bool reported = false;
			if (!reported)
			{
				reported = true;
				LOG(WARNING) << "[game/WARN] " << formatted
				             << "  (pre-existing engine issue, also present without the loader; "
				                "further occurrences suppressed)";
			}
			return;
		}

		// Script errors are always surfaced - they are the whole reason this
		// hook exists. Everything else honours the config toggle.
		const bool is_script_error = formatted.starts_with("Script er");
		if (!is_script_error && g_output_vanilla_log && !g_output_vanilla_log->get_value())
		{
			return;
		}

		const char* label       = nullptr;
		const al::eLogLevel lvl = map_level(level, label);

		LOG(lvl) << "[game/" << label << "] [" << basename_of(filename) << ":" << line_number << "] " << formatted;
	}
} // namespace

namespace big::hades1
{
	bool install_log_write_hook()
	{
		const auto log_write_addr = game_symbol("Log::Write");
		if (!log_write_addr)
		{
			LOG(WARNING) << "Log::Write not in the symbol map; the engine's own log stays invisible.";
			return false;
		}

		g_output_vanilla_log = config::general->bind("Logging",
		                                            "Output Vanilla Game Log",
		                                            true,
		                                            "Mirror the game's own log output into the Hell1Modding log. "
		                                            "Script errors are always shown regardless of this setting.");

		g_log_write_hook = safetyhook::create_inline(reinterpret_cast<void*>(log_write_addr),
		                                            reinterpret_cast<void*>(&log_write_detour));

		if (!g_log_write_hook)
		{
			LOG(ERROR) << "safetyhook refused Log::Write.";
			return false;
		}

		LOG(INFO) << "Hooked Log::Write; mirroring the game's log.";
		return true;
	}
} // namespace big::hades1
