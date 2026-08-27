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
		// Engine INFO is pure chatter - asset reads, sampler descriptors, a few
		// hundred lines a boot - so it lands at DEBUG and stays in the log file
		// rather than the console. Engine warnings and errors keep their level.
		case 4:  out_label = "INFO"; return DEBUG;
		case 8:  out_label = "WARN"; return WARNING;
		case 16: out_label = "ERR";  return ERROR;
		default: out_label = "UNK";  return DEBUG;
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

		// The engine probes for optional files it does not ship and logs every
		// miss at ERROR: weapons the profile has not unlocked
		// (FistWeapon.pkg_manifest, GunWeapon.pkg_manifest), the per-machine
		// config override named after the hostname, optional UI .sjson
		// overrides. 15 of the 18 ERROR lines in a clean boot are this, and on
		// a healthy install every one of them is normal - so they would train
		// a user to ignore the console entirely.
		//
		// They stay in the log file at DEBUG, and the first one is reported so
		// the behaviour is discoverable rather than silently swallowed. Only
		// this exact shape is demoted: a *read* that failed because the file
		// is not there. Anything else the engine calls an error stays one.
		if (level == 16 && formatted.starts_with("Error opening file:") && formatted.contains(" -- rb ")
		    && formatted.contains("No such file or directory"))
		{
			static bool reported = false;
			if (!reported)
			{
				reported = true;
				LOG(WARNING) << "The engine probes for optional files that do not exist and logs each miss as an "
				                "error. These are normal; they are recorded in LogOutput.log at DEBUG level and "
				                "kept off the console from here on.";
			}

			LOG(DEBUG) << "[game/ERR] [" << basename_of(filename) << ":" << line_number << "] " << formatted;
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

		// Collapse runs of the identical message, the way syslog does.
		//
		// The engine repeats itself in bursts when something is unhappy:
		// "bink.cpp:1719 Task overrun: ZagreusIdle_Bink" arrived 102 times in
		// 2.2 seconds, one per dropped decode. Every line said the same thing,
		// and 102 of them buries anything else on the console.
		//
		// Nothing is hidden - the count is reported as soon as a different
		// message shows up, so the burst is still visible and still countable,
		// it just occupies two lines instead of a hundred. Deliberately
		// consecutive-only: two messages alternating are both worth seeing.
		{
			static std::mutex mutex;
			static std::string last;
			static al::eLogLevel last_level = INFO;
			static size_t repeats           = 0;

			std::string line = std::format("[game/{}] [{}:{}] {}", label, basename_of(filename), line_number, formatted);

			std::scoped_lock lock(mutex);

			if (line == last)
			{
				++repeats;
				return;
			}

			if (repeats)
			{
				LOG(last_level) << "  (previous message repeated " << repeats << " more time" << (repeats == 1 ? "" : "s") << ")";
				repeats = 0;
			}

			last       = line;
			last_level = lvl;

			LOG(lvl) << line;
		}
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

		LOG(DEBUG) << "Hooked Log::Write; mirroring the game's log.";
		return true;
	}
} // namespace big::hades1
