#include "common.hpp"

#include "file_manager/file_manager.hpp"
#include "legacy_packages.hpp"
#include "lua_extensions/bindings/paths_ext.hpp"

namespace big::hades1
{
	namespace
	{
		// Content-relative targets, forward slashes, as written in the modfile.
		std::set<std::string> g_installed_last_run;
		std::set<std::string> g_installed_this_run;

		std::filesystem::path content_folder()
		{
			return lua::paths_ext::get_game_executable_folder().parent_path() / "Content";
		}

		// Kept in the profile, not the game folder, so two r2modman profiles
		// cannot disagree about what is installed.
		std::filesystem::path record_path()
		{
			return g_file_manager.get_project_file("./legacy_packages.txt").get_path();
		}

		bool is_package(const std::string& target)
		{
			const auto dot = target.rfind('.');
			if (dot == std::string::npos)
			{
				return false;
			}

			std::string ext = target.substr(dot);
			std::transform(ext.begin(),
			               ext.end(),
			               ext.begin(),
			               [](unsigned char c)
			               {
				               return static_cast<char>(std::tolower(c));
			               });

			return ext == ".pkg" || ext == ".pkg_manifest";
		}

		std::string to_forward_slashes(std::string s)
		{
			std::replace(s.begin(), s.end(), '\\', '/');
			return s;
		}

		// Copying a .pkg is hundreds of megabytes, so only when it has actually
		// changed. Size plus write time is what every installer uses and is
		// enough here: the source is a file the user unpacked, not something
		// being edited in place.
		bool needs_copy(const std::filesystem::path& source, const std::filesystem::path& destination)
		{
			std::error_code ec;

			if (!std::filesystem::exists(destination, ec))
			{
				return true;
			}

			if (std::filesystem::file_size(source, ec) != std::filesystem::file_size(destination, ec))
			{
				return true;
			}

			return std::filesystem::last_write_time(source, ec) != std::filesystem::last_write_time(destination, ec);
		}
	}

	void begin_package_install()
	{
		g_installed_last_run.clear();
		g_installed_this_run.clear();

		std::ifstream file(record_path());
		if (!file)
		{
			return;
		}

		std::string line;
		while (std::getline(file, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			{
				line.pop_back();
			}

			if (!line.empty())
			{
				g_installed_last_run.insert(line);
			}
		}
	}

	bool install_package(const std::string& target, const std::filesystem::path& source, const std::string& owner)
	{
		if (!is_package(target))
		{
			return false;
		}

		const std::string key = to_forward_slashes(target);
		const auto destination = content_folder() / std::filesystem::path(key);

		std::error_code ec;

		// Never overwrite a file the loader did not put there. A mod whose
		// modfile Replaces one of the game's own packages is handled by
		// file_redirect instead, which serves the mod's copy at open time and
		// leaves the original intact - the whole reason this loader does not
		// need ModImporter's backup-and-restore dance.
		if (std::filesystem::exists(destination, ec) && !g_installed_last_run.contains(key))
		{
			LOGF(DEBUG,
			     "Content/Mods: {} replaces the game's own \"{}\"; serving it from the mod folder rather than "
			     "overwriting it.",
			     owner,
			     key);
			return false;
		}

		std::filesystem::create_directories(destination.parent_path(), ec);

		if (needs_copy(source, destination))
		{
			std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);

			if (ec)
			{
				LOGF(ERROR,
				     "Content/Mods: {} could not install \"{}\": {}. The package will not load.",
				     owner,
				     key,
				     ec.message());
				return false;
			}

			// Match the timestamp so needs_copy() is stable next launch.
			const auto when = std::filesystem::last_write_time(source, ec);
			if (!ec)
			{
				std::filesystem::last_write_time(destination, when, ec);
			}

			LOGF(INFO, "Content/Mods: {} installed \"{}\" into the game folder.", owner, key);
		}

		g_installed_this_run.insert(key);
		return true;
	}

	void finish_package_install()
	{
		std::error_code ec;

		// Anything installed before but not wanted now belongs to a mod that
		// has been removed or changed. Delete it, so uninstalling a mod really
		// does leave the game folder as it was.
		for (const auto& stale : g_installed_last_run)
		{
			if (g_installed_this_run.contains(stale))
			{
				continue;
			}

			const auto path = content_folder() / std::filesystem::path(stale);
			if (std::filesystem::remove(path, ec))
			{
				LOGF(INFO, "Content/Mods: removed \"{}\", whose mod is no longer installed.", stale);
			}
		}

		std::ofstream file(record_path(), std::ios::trunc);
		if (!file)
		{
			LOG(WARNING) << "Could not write the legacy package record; installed packages may not be cleaned up.";
			return;
		}

		for (const auto& entry : g_installed_this_run)
		{
			file << entry << '\n';
		}
	}
}
