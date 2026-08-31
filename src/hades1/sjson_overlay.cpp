#include "common.hpp"

#include <shared_mutex>

#include "lua_extensions/bindings/paths_ext.hpp"
#include "sjson_overlay.hpp"

namespace big::hades1::sjson_overlay
{
	namespace
	{
		// Every .sjson the game ships, as logical paths ("Game/GUI/Fx.sjson"),
		// built by walking Content/Game at startup.
		//
		// This replaces the hand-written directory whitelist Hell2Modding uses.
		// A list of directories cannot catch the mistake that actually matters
		// here: Hades 1 ships MainMenuScreen.sjson and InGameUI.sjson in *two*
		// places each - Game/ and Game/GUI/ - so a mod that targets the wrong
		// one lands in a perfectly valid directory and silently does nothing.
		// Matching against the real file list catches that, and cannot go stale
		// when a game update moves or adds a file.
		std::set<std::string> g_game_files;

		// leaf name -> every logical path shipping under that name, so a
		// mismatch can name the paths the author probably meant.
		std::unordered_map<std::string, std::vector<std::string>> g_game_files_by_leaf;

		std::unordered_map<std::string, std::string> g_path_index;
		std::set<std::string> g_enumerated;
		std::shared_mutex g_mutex;

		std::string to_lower(std::string s)
		{
			std::transform(s.begin(),
			               s.end(),
			               s.begin(),
			               [](unsigned char c)
			               {
				               return static_cast<char>(std::tolower(c));
			               });
			return s;
		}

		void register_file(const std::string& logical_relpath, const std::string& absolute_path)
		{
			const std::string normalized = normalize_path(logical_relpath);

			const size_t last_slash = normalized.rfind('/');
			const std::string subdir   = last_slash == std::string::npos ? "" : normalized.substr(0, last_slash);
			const std::string filename = last_slash == std::string::npos ? normalized : normalized.substr(last_slash + 1);

			const size_t dot = filename.rfind('.');
			const std::string extension = dot == std::string::npos ? "" : filename.substr(dot);

			if (to_lower(extension) != ".sjson")
			{
				LOGF(WARNING, "SJSON overlay: only .sjson files are supported here; ignoring {}", normalized);
				return;
			}

			// A warning, not a rejection - the file is still served. The game
			// could legitimately gain a file we have not indexed, and a mod
			// author is better served by a loud hint than by a refusal.
			if (!g_game_files.contains(to_lower(normalized)))
			{
				const auto candidates = g_game_files_by_leaf.find(to_lower(filename));

				if (candidates == g_game_files_by_leaf.end())
				{
					LOGF(WARNING,
					     "SJSON overlay: the game ships no \"{}\", so this file will never be loaded. Check the "
					     "path and spelling against Content/Game.",
					     normalized);
				}
				else
				{
					// The common failure: right filename, wrong directory.
					// Hades 1 ships MainMenuScreen.sjson and InGameUI.sjson
					// under both Game/ and Game/GUI/.
					std::string paths;
					for (const auto& candidate : candidates->second)
					{
						paths += (paths.empty() ? "" : ", ") + candidate;
					}

					LOGF(WARNING,
					     "SJSON overlay: the game has no \"{}\", but does ship that filename at: {}. This overlay "
					     "will not be loaded until its path matches one of those.",
					     normalized,
					     paths);
				}
			}

			// Indexed lower-case: lookups come from a path the engine assembled,
			// whose casing follows the engine's own spelling rather than the
			// mod author's directory names.
			const std::string key = to_lower(normalized);

			std::unique_lock lock(g_mutex);

			if (const auto it = g_path_index.find(key); it != g_path_index.end())
			{
				if (it->second != absolute_path)
				{
					LOGF(WARNING,
					     "SJSON overlay: \"{}\" is already provided by {}; ignoring {}",
					     normalized,
					     it->second,
					     absolute_path);
				}
				return;
			}

			g_path_index[key] = absolute_path;

			const std::string enum_key = to_lower(subdir) + "|" + to_lower(extension);
			if (g_enumerated.contains(enum_key))
			{
				LOGF(WARNING,
				     "SJSON overlay: \"{}\" was registered after the engine had already enumerated {}; it will not "
				     "take effect until the next launch.",
				     normalized,
				     subdir);
			}

			LOGF(DEBUG, "SJSON overlay: {} -> {}", normalized, absolute_path);
		}

		// Walk Content/Game once and record every .sjson it ships.
		//
		// Deliberately the whole tree rather than a curated set of directories:
		// that picks up all 11 locale folders under Game/Text, the files that
		// sit directly in Game/Text (Aliases.sjson, Languages.sjson), and
		// anything a game update adds, with nothing to keep in sync by hand.
		void index_game_content()
		{
			const auto content = lua::paths_ext::get_game_executable_folder().parent_path() / "Content";
			const auto game    = content / "Game";

			std::error_code ec;
			if (!std::filesystem::is_directory(game, ec))
			{
				LOGF(WARNING,
				     "SJSON overlay: {} is not there, so mod overlays cannot be checked against the game's own files.",
				     reinterpret_cast<const char*>(game.u8string().c_str()));
				return;
			}

			auto options = std::filesystem::directory_options::skip_permission_denied
			    | std::filesystem::directory_options::follow_directory_symlink;

			for (std::filesystem::recursive_directory_iterator it(game, options, ec), end; it != end; it.increment(ec))
			{
				if (ec)
				{
					ec.clear();
					continue;
				}

				if (!it->is_regular_file(ec) || to_lower(it->path().extension().string()) != ".sjson")
				{
					continue;
				}

				const auto relative = std::filesystem::relative(it->path(), content, ec);
				if (ec)
				{
					ec.clear();
					continue;
				}

				const std::u8string u8rel = relative.u8string();
				const std::string logical =
				    to_lower(normalize_path(std::string(reinterpret_cast<const char*>(u8rel.c_str()), u8rel.size())));

				g_game_files.insert(logical);

				const size_t slash = logical.rfind('/');
				g_game_files_by_leaf[slash == std::string::npos ? logical : logical.substr(slash + 1)].push_back(logical);
			}

			LOGF(DEBUG, "SJSON overlay: indexed {} .sjson file(s) shipped under Content/Game.", g_game_files.size());
		}

		void scan_one_mod(const std::filesystem::path& overlay_root)
		{
			std::error_code ec;

			auto options = std::filesystem::directory_options::skip_permission_denied
			    | std::filesystem::directory_options::follow_directory_symlink;

			for (std::filesystem::recursive_directory_iterator it(overlay_root, options, ec), end; it != end;
			     it.increment(ec))
			{
				if (ec)
				{
					LOGF(WARNING, "SJSON overlay: {}", ec.message());
					ec.clear();
					continue;
				}

				if (!it->is_regular_file(ec) || to_lower(it->path().extension().string()) != ".sjson")
				{
					continue;
				}

				const auto relative = std::filesystem::relative(it->path(), overlay_root, ec);
				if (ec)
				{
					ec.clear();
					continue;
				}

				// The overlay directory stands in for Content/Game, so a file
				// at <overlay>/Animations/Fx.sjson is Game/Animations/Fx.sjson.
				const std::u8string u8rel = relative.u8string();
				const std::u8string u8abs = it->path().u8string();

				register_file("Game/" + normalize_path(std::string(reinterpret_cast<const char*>(u8rel.c_str()), u8rel.size())),
				              std::string(reinterpret_cast<const char*>(u8abs.c_str()), u8abs.size()));
			}
		}
	}

	std::string normalize_path(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');

		while (!path.empty() && path.back() == '/')
		{
			path.pop_back();
		}

		return path;
	}

	void scan_all_plugin_data(const std::filesystem::path& plugins_data_path)
	{
		std::error_code ec;
		if (!std::filesystem::exists(plugins_data_path, ec))
		{
			return;
		}

		index_game_content();

		for (std::filesystem::directory_iterator it(plugins_data_path, ec), end; it != end; it.increment(ec))
		{
			if (ec)
			{
				ec.clear();
				continue;
			}

			if (!it->is_directory(ec))
			{
				continue;
			}

			const auto overlay = it->path() / DATA_DIR_NAME;
			if (std::filesystem::is_directory(overlay, ec))
			{
				scan_one_mod(overlay);
			}
		}

		std::shared_lock lock(g_mutex);
		if (!g_path_index.empty())
		{
			LOGF(INFO, "SJSON overlay: {} file(s) from mods will replace the game's own.", g_path_index.size());
		}
	}

	std::string lookup(const std::string& logical_relpath)
	{
		std::shared_lock lock(g_mutex);

		const auto it = g_path_index.find(logical_relpath);
		return it == g_path_index.end() ? std::string{} : it->second;
	}

	void mark_directory_enumerated(const std::string& normalized_subdir, const std::string& extension)
	{
		std::unique_lock lock(g_mutex);
		g_enumerated.insert(to_lower(normalized_subdir) + "|" + to_lower(extension));
	}

	bool any_registered()
	{
		std::shared_lock lock(g_mutex);
		return !g_path_index.empty();
	}
}
