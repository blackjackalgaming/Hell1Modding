#include "common.hpp"

#include "config/config.hpp"
#include "file_redirect.hpp"
#include "legacy_config.hpp"
#include "legacy_mods.hpp"
#include "lua_extensions/bindings/paths_ext.hpp"
#include "script_hook.hpp"

// Loading ModImporter-style mods from Content/Mods.
//
// Every Hades 1 mod that exists today is shaped for ModImporter: a folder under
// Content/Mods holding a modfile.txt, and no manifest.json. RoMBase only scans
// ReturnOfModding/plugins and only accepts folders that have a manifest, so
// without this the entire existing ecosystem is invisible to the loader.
//
// SGG_Modding-DemonDaemon does not cover this. Its `auto()` reads the modfile
// belonging to *the plugin that called it* - "parse my own modfile", not "find
// mods" - so it only helps a mod that is already a RoM plugin. Discovery was
// ModImporter's job and nothing replaced it. Hell2Modding has no equivalent
// either: Hades 2 never had a ModImporter era, so there was no prior corpus to
// stay compatible with.
//
// ModImporter has three payload directives, and `To` sets the target for
// whichever follows:
//
//   Import / Top Import   Lua, into a script under Scripts/
//   SJSON                 merged into a game data file
//   Replace               substitutes a game file wholesale - this is how
//                         .pkg, .pkg_manifest, .thing_bin, .bik, .fsb and
//                         subtitle .csv all get in
//
// **Everything is done in memory.** ModImporter rewrote the game's own files
// and kept backups, because it was a launcher with no way into the process. We
// are inside it, so Import goes through the Load hook and Replace/SJSON go
// through a file-open redirect. Nothing under Content/ is ever written. That
// matters most for a mod like Goddess Codex, which Replaces 71 files including
// audio banks and movies: on disk that is 71 backups to keep straight, a Steam
// file verification away from being wiped, and one mid-write crash away from an
// install that will not start.

namespace big::hades1
{
	namespace
	{
		// Priority is per *command*, not per mod. modimporter.py keeps one
		// deque per target file, appends a record per command carrying the
		// `ep` in force at that point, then sorts the whole lot by ep. So a
		// single modfile can raise its priority halfway through, and Replace,
		// SJSON and Import all interleave in one ordered sequence per target.
		constexpr int default_priority = 100;

		struct legacy_import
		{
			std::string target;      // script filename, e.g. "RoomManager.lua"
			std::string engine_path; // as ScriptManager::Load wants it, "../Mods/..."
			std::filesystem::path source;
			int priority = default_priority;
			bool top     = false;
		};

		struct legacy_file
		{
			std::string target; // relative to Content, e.g. "Win/Packages/X.pkg"
			std::filesystem::path source;
			int priority = default_priority;
			bool merge   = false; // SJSON merges; Replace substitutes
		};

		struct legacy_mod
		{
			std::string name;
			std::filesystem::path folder;
			int priority   = default_priority; // lowest seen, for display only
			size_t missing = 0;
			std::vector<legacy_import> imports;
			std::vector<legacy_file> files;
		};

		std::vector<legacy_mod> g_legacy_mods;
		toml_v2::config_file::config_entry<bool>* g_enabled = nullptr;

		std::string trim(std::string_view s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
			{
				s.remove_prefix(1);
			}
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
			{
				s.remove_suffix(1);
			}
			return std::string(s);
		}

		// modfile.txt is token-based, not line-based. Reproduced from
		// modimporter.py's splitlines()/tokenise():
		//
		//   ::            comment to end of line
		//   -: ... :-     block comment
		//   ;             separates several commands on one line
		//   " "           quoted text is one token, and is exempt from all of
		//                 the above
		//   space, comma  both separate tokens, so `Import a.lua, b.lua` is two
		//                 commands and `To "A.lua","B.lua"` is two targets
		//
		// Reading it a line at a time - which is what this used to do - silently
		// drops everything after a `;` and treats a multi-argument command as
		// one argument.
		std::vector<std::string> split_commands(const std::string& body)
		{
			std::vector<std::string> lines{""};
			bool in_quotes = false;
			bool in_block  = false;

			for (size_t i = 0; i < body.size(); ++i)
			{
				const char c    = body[i];
				const char next = (i + 1 < body.size()) ? body[i + 1] : '\0';

				if (in_block)
				{
					if (c == ':' && next == '-')
					{
						in_block = false;
						++i;
					}
					continue;
				}

				if (c == '\n')
				{
					in_quotes = false;
					lines.emplace_back();
					continue;
				}

				if (in_quotes)
				{
					if (c == '"')
					{
						in_quotes = false;
					}
					lines.back().push_back(c);
					continue;
				}

				if (c == '"')
				{
					in_quotes = true;
					lines.back().push_back(c);
					continue;
				}

				if (c == ':' && next == ':')
				{
					// Comment: skip to end of line.
					while (i < body.size() && body[i] != '\n')
					{
						++i;
					}
					lines.emplace_back();
					continue;
				}

				if (c == '-' && next == ':')
				{
					in_block = true;
					++i;
					continue;
				}

				if (c == ';')
				{
					lines.emplace_back();
					continue;
				}

				lines.back().push_back(c);
			}

			return lines;
		}

		// Quoted runs survive whole; everything else splits on spaces and
		// commas alike.
		std::vector<std::string> tokenise(const std::string& line)
		{
			std::vector<std::string> tokens;
			std::string current;
			bool in_quotes = false;

			auto flush = [&]
			{
				if (!current.empty())
				{
					tokens.push_back(current);
					current.clear();
				}
			};

			for (const char c : line)
			{
				if (c == '"')
				{
					in_quotes = !in_quotes;
					continue;
				}

				if (!in_quotes && (c == ' ' || c == '\t' || c == ',' || c == '\r'))
				{
					flush();
					continue;
				}

				current.push_back(c);
			}

			flush();
			return tokens;
		}

		bool starts_with_keyword(const std::vector<std::string>& tokens, std::initializer_list<const char*> keyword)
		{
			if (tokens.size() < keyword.size())
			{
				return false;
			}

			size_t i = 0;
			for (const char* word : keyword)
			{
				if (_stricmp(tokens[i++].c_str(), word) != 0)
				{
					return false;
				}
			}

			return true;
		}

		std::string to_forward_slashes(std::string s)
		{
			std::replace(s.begin(), s.end(), '\\', '/');
			return s;
		}

		// Where the engine looks, spelled the way ScriptManager::Load wants it.
		// Load resolves relative to Content/Scripts, which is exactly why
		// ModImporter's inserted lines read `Import "../Mods/<Mod>/<file>"`.
		std::string engine_script_path(const std::filesystem::path& content, const std::filesystem::path& source)
		{
			std::error_code ec;
			const auto rel = std::filesystem::relative(source, content, ec);
			if (ec || rel.empty())
			{
				return {};
			}

			return "../" + to_forward_slashes(rel.string());
		}

		// One modfile. `to` and `priority` are deliberately *not* inherited
		// from the including file: modimporter.py resets both at the top of
		// every loadmodfile(), so an Include starts again at the game default
		// (Scripts/RoomManager.lua for Hades) and priority 100.
		void parse_modfile(const std::filesystem::path& modfile,
		                   const std::filesystem::path& content,
		                   legacy_mod& mod,
		                   int depth)
		{
			if (depth > 8)
			{
				LOGF(WARNING, "{}: modfile Include nested too deeply; stopping.", mod.name);
				return;
			}

			std::ifstream stream(modfile, std::ios::binary);
			if (!stream)
			{
				LOGF(WARNING, "{}: cannot read {}", mod.name, to_forward_slashes(modfile.string()));
				return;
			}

			const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

			// Source paths are relative to the modfile that declares them, not
			// to the mod root: Goddess Codex's Common/modfile_common.txt says
			// `Import "SeparateGoddess.lua"` for Common/SeparateGoddess.lua and
			// `Replace "../Audio/VO.h"` to climb back out.
			const auto base = modfile.parent_path();

			std::vector<std::string> targets{"RoomManager.lua"};
			int priority = default_priority;

			for (const auto& line : split_commands(body))
			{
				const auto tokens = tokenise(line);
				if (tokens.empty())
				{
					continue;
				}

				auto arguments = [&](size_t skip)
				{
					return std::vector<std::string>(tokens.begin() + skip, tokens.end());
				};

				auto add_files = [&](size_t skip, bool merge)
				{
					for (const auto& target : targets)
					{
						for (const auto& argument : arguments(skip))
						{
							mod.files.push_back({target,
							                     std::filesystem::weakly_canonical(base / argument),
							                     priority,
							                     merge});
						}
					}
				};

				if (starts_with_keyword(tokens, {"To"}))
				{
					targets.clear();
					for (const auto& argument : arguments(1))
					{
						std::string target = to_forward_slashes(argument);
						if (_strnicmp(target.c_str(), "Scripts/", 8) == 0)
						{
							target = target.substr(8);
						}
						targets.push_back(target);
					}

					// `To` with no argument resets to the game's default.
					if (targets.empty())
					{
						targets.push_back("RoomManager.lua");
					}
				}
				else if (starts_with_keyword(tokens, {"Load", "Priority"}) || starts_with_keyword(tokens, {"Priority"}))
				{
					const size_t skip = starts_with_keyword(tokens, {"Load", "Priority"}) ? 2 : 1;
					priority          = default_priority;

					if (tokens.size() > skip)
					{
						try
						{
							priority = std::stoi(tokens[skip]);
						}
						catch (const std::exception&)
						{
							// ModImporter ignores an unreadable value and keeps
							// the default rather than failing the modfile.
						}
					}
				}
				else if (starts_with_keyword(tokens, {"Include"}))
				{
					for (const auto& argument : arguments(1))
					{
						const auto included = std::filesystem::weakly_canonical(base / argument);

						std::error_code ec;
						if (std::filesystem::is_directory(included, ec))
						{
							// An Include naming a folder loads every modfile in it.
							for (const auto& entry : std::filesystem::directory_iterator(included, ec))
							{
								if (!ec && entry.is_regular_file(ec))
								{
									parse_modfile(entry.path(), content, mod, depth + 1);
								}
							}
						}
						else
						{
							parse_modfile(included, content, mod, depth + 1);
						}
					}
				}
				else if (starts_with_keyword(tokens, {"Top", "Import"}) || starts_with_keyword(tokens, {"Import"}))
				{
					const bool top    = starts_with_keyword(tokens, {"Top", "Import"});
					const size_t skip = top ? 2 : 1;

					for (const auto& target : targets)
					{
						for (const auto& argument : arguments(skip))
						{
							const auto source = std::filesystem::weakly_canonical(base / argument);
							mod.imports.push_back(
							    {target, engine_script_path(content, source), source, priority, top});
						}
					}
				}
				else if (starts_with_keyword(tokens, {"Replace"}))
				{
					add_files(1, false);
				}
				else if (starts_with_keyword(tokens, {"SJSON"}))
				{
					add_files(1, true);
				}
				else if (starts_with_keyword(tokens, {"XML"}) || starts_with_keyword(tokens, {"CSV"})
				         || starts_with_keyword(tokens, {"Map"}))
				{
					// Real ModImporter directives we have not built. Named
					// individually so a mod author can see which part of their
					// mod is inert rather than guessing.
					LOGF(WARNING,
					     "{}: \"{}\" is a directive Hell1Modding does not support yet; that part of the mod will not "
					     "apply.",
					     mod.name,
					     tokens[0]);
				}
				else
				{
					// ModImporter raises and abandons the whole modfile here.
					// Warning and carrying on is friendlier and means one bad
					// line cannot cost a user the rest of a large mod.
					LOGF(WARNING, "{}: unrecognised modfile command \"{}\"; ignoring it.", mod.name, trim(line));
				}
			}
		}
	}

	void scan_legacy_mods()
	{
		g_legacy_mods.clear();
		clear_file_redirects();

		if (!g_enabled)
		{
			g_enabled = config::general->bind("Mods",
			                                  "Load Content Mods Folder",
			                                  true,
			                                  "Load ModImporter-style mods from Content/Mods, which is the layout "
			                                  "every existing Hades 1 mod uses. Turn this off to load only "
			                                  "ReturnOfModding plugins.");
		}

		if (!g_enabled->get_value())
		{
			return;
		}

		const auto content     = lua::paths_ext::get_game_executable_folder().parent_path() / "Content";
		const auto mods_folder = content / "Mods";

		std::error_code ec;
		if (!std::filesystem::is_directory(mods_folder, ec))
		{
			return;
		}

		for (const auto& entry : std::filesystem::directory_iterator(mods_folder, ec))
		{
			if (ec || !entry.is_directory(ec))
			{
				continue;
			}

			const auto modfile = entry.path() / "modfile.txt";
			if (!std::filesystem::is_regular_file(modfile, ec))
			{
				continue;
			}

			legacy_mod mod;
			mod.name   = entry.path().filename().string();
			mod.folder = entry.path();

			parse_modfile(modfile, content, mod, 0);

			if (mod.imports.empty() && mod.files.empty())
			{
				LOGF(WARNING, "{}: modfile.txt declares nothing to do; skipping.", mod.name);
				continue;
			}

			for (const auto& e : mod.imports) mod.priority = std::min(mod.priority, e.priority);
			for (const auto& e : mod.files)   mod.priority = std::min(mod.priority, e.priority);

			for (const auto& import : mod.imports)
			{
				if (!std::filesystem::is_regular_file(import.source, ec))
				{
					++mod.missing;
				}
			}
			for (const auto& file : mod.files)
			{
				if (!std::filesystem::is_regular_file(file.source, ec))
				{
					++mod.missing;
				}
			}

			g_legacy_mods.push_back(std::move(mod));
		}

		// ModImporter orders by Load Priority, lower first. Name breaks ties so
		// the sequence is identical on every machine.
		std::sort(g_legacy_mods.begin(),
		          g_legacy_mods.end(),
		          [](const legacy_mod& a, const legacy_mod& b)
		          {
			          return (a.priority != b.priority) ? a.priority < b.priority : a.name < b.name;
		          });

		// Replace and SJSON are answered when the engine opens the file, so
		// they are registered now rather than fired at a script boundary.
		for (const auto& mod : g_legacy_mods)
		{
			for (const auto& file : mod.files)
			{
				if (!std::filesystem::is_regular_file(file.source, ec))
				{
					LOGF(ERROR,
					     "Content/Mods: {} refers to \"{}\", which does not exist. Check the path in its modfile.",
					     mod.name,
					     to_forward_slashes(file.source.string()));
					continue;
				}

				if (file.merge)
				{
					// SJSON merging needs an sjson parser; not built yet. Say
					// so rather than let the mod look like it fully applied.
					LOGF(WARNING,
					     "Content/Mods: {} merges SJSON into \"{}\", which is not supported yet; that part will not "
					     "apply.",
					     mod.name,
					     file.target);
					continue;
				}

				add_file_redirect(file.target, file.source, mod.name);
			}
		}

		for (const auto& mod : g_legacy_mods)
		{
			LOGF(mod.missing ? WARNING : INFO,
			     "Content/Mods: {} (priority {}, {} import(s), {} file(s){})",
			     mod.name,
			     mod.priority,
			     mod.imports.size(),
			     mod.files.size(),
			     mod.missing ? std::format(", {} MISSING", mod.missing) : std::string());
		}
	}

	void fire_legacy_imports(const char* script, bool top)
	{
		if (!script || g_legacy_mods.empty())
		{
			return;
		}

		std::error_code ec;

		for (const auto& mod : g_legacy_mods)
		{
			for (const auto& import : mod.imports)
			{
				if (import.top != top || _stricmp(import.target.c_str(), script) != 0)
				{
					continue;
				}

				// Checked here rather than left to the engine: a failed open
				// logs "Error opening file: ... -- rb (No such file or
				// directory)", which is the exact shape log_write.cpp demotes
				// to DEBUG to suppress the engine's optional-file probing. A
				// mistyped filename - the commonest mod-authoring mistake -
				// would otherwise produce nothing at all on the console.
				if (import.engine_path.empty() || !std::filesystem::is_regular_file(import.source, ec))
				{
					LOGF(ERROR,
					     "Content/Mods: {} refers to \"{}\", which does not exist. Check the path in its modfile. "
					     "Skipping that import; the rest of the mod still loads.",
					     mod.name,
					     to_forward_slashes(import.source.string()));
					continue;
				}

				// A config.lua gets the Chalk treatment: watch what it adds to
				// _G, expose it as config/<Mod>.cfg, and feed the .cfg's values
				// back before the mod's own code reads them. The window is
				// exactly here - config.lua is a Top Import, so it has run and
				// the mod's main file has not.
				const bool is_config = _stricmp(import.source.filename().string().c_str(), "config.lua") == 0;

				if (is_config)
				{
					snapshot_globals();
				}

				LOGF(INFO, "Content/Mods: {} importing {}", mod.name, import.engine_path);
				load_game_script(import.engine_path.c_str());

				if (is_config)
				{
					bind_legacy_config(mod.name);
				}
			}
		}
	}

	std::vector<legacy_mod_info> get_legacy_mods()
	{
		std::vector<legacy_mod_info> out;
		out.reserve(g_legacy_mods.size());

		for (const auto& mod : g_legacy_mods)
		{
			out.push_back({mod.name, mod.priority, mod.imports.size() + mod.files.size(), mod.missing});
		}

		return out;
	}
}
