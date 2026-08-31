#include "common.hpp"

#include "file_manager/file_manager.hpp"
#include "hades_lua.hpp"
#include "plugin_data_files.hpp"
#include "sjson_overlay.hpp"

#include <safetyhook.hpp>

namespace big::hades1
{
	namespace
	{
		SafetyHookInline g_enumerate_hook{};
		SafetyHookInline g_append_path_hook{};

		// filename -> absolute path on disk. Written once at startup, read from
		// the engine's file threads afterwards, so it is never mutated once the
		// hook is live.
		std::unordered_map<std::string, std::string> g_assets;

		// The extensions the engine will actually open. Taken from what Hades 1
		// ships rather than from Hell2Modding's list: no .gpk here, because
		// Hades 1 uses .sga and has no Granny archives at all.
		bool is_engine_asset(const std::filesystem::path& extension)
		{
			static const std::set<std::string> known = {
			    ".pkg",
			    ".pkg_manifest",
			    ".map_text",
			    ".thing_bin",
			    ".thing_text",
			    ".csv",
			    ".sga",
			    ".bank",
			    ".fsb",
			    ".bik",
			    ".bik_atlas",
			};

			std::string ext = reinterpret_cast<const char*>(extension.u8string().c_str());
			std::transform(ext.begin(),
			               ext.end(),
			               ext.begin(),
			               [](unsigned char c)
			               {
				               return static_cast<char>(std::tolower(c));
			               });

			return known.contains(ext);
		}

		// eastl::string is 0x18 bytes here - the ABI the whole project is built
		// to match, see CLAUDE.md. Only used to report how many entries the
		// engine found; nothing is written.
		size_t vector_count(const void* eastl_vector)
		{
			if (!eastl_vector)
			{
				return 0;
			}

			const auto* words = static_cast<const uintptr_t*>(eastl_vector);
			const uintptr_t begin = words[0];
			const uintptr_t end   = words[1];

			if (!begin || end < begin)
			{
				return 0;
			}

			return (end - begin) / 0x18;
		}

		// Substitute an SJSON overlay, keyed on the path the engine has just
		// assembled rather than on the filename it asked for.
		//
		// `output` is fully qualified by this point, so the Content-relative
		// part is whatever follows the last "Content/" in it - the same
		// derivation Hell2Modding uses.
		void substitute_sjson_overlay(char* output)
		{
			if (!sjson_overlay::any_registered() || !output)
			{
				return;
			}

			const std::string normalized = sjson_overlay::normalize_path(output);

			std::string lower = normalized;
			std::transform(lower.begin(),
			               lower.end(),
			               lower.begin(),
			               [](unsigned char c)
			               {
				               return static_cast<char>(std::tolower(c));
			               });

			static constexpr std::string_view marker = "content/";
			const size_t at = lower.rfind(marker);
			if (at == std::string::npos)
			{
				return;
			}

			const std::string replacement = sjson_overlay::lookup(lower.substr(at + marker.size()));
			if (replacement.empty())
			{
				return;
			}

			constexpr size_t fs_max_path = 512;
			if (replacement.size() + 1 > fs_max_path)
			{
				LOGF(WARNING, "SJSON overlay: path too long for the engine's buffer, skipping {}", replacement);
				return;
			}

			memcpy(output, replacement.c_str(), replacement.size() + 1);

			static std::set<std::string> reported;
			static std::mutex reported_mutex;
			{
				std::scoped_lock lock(reported_mutex);
				if (reported.insert(replacement).second)
				{
					LOGF(INFO, "SJSON overlay: serving {}", replacement);
				}
			}
		}

		// The Forge assembles every asset path through this, and the engine then
		// opens whatever it produced. Substituting `output` here is the only
		// place an absolute path outside the Content tree survives.
		//
		// PlatformOpenFile - which the legacy Content/Mods redirect uses - is
		// too late and too constrained: by then the path has been resolved
		// against a ResourceDirectory, and a replacement can only be expressed
		// relative to that same directory. That works for Content/Mods, which
		// lives inside Content; it cannot reach plugins_data, which does not.
		// Measured, before this hook existed: the engine turned our relative
		// path into Content\Win\<profile>\...\GUI.pkg and reported no such
		// file. This is why Hell2Modding hooks fsAppendPathComponent and not
		// just the file open.
		void append_path_detour(const char* base_path, const char* path_component, char* output)
		{
			g_append_path_hook.call<void, const char*, const char*, char*>(base_path, path_component, output);

			if (!path_component || !*path_component || !output)
			{
				return;
			}

			// The engine asks by bare filename for packages, and with a
			// subdirectory prefix for some other kinds, so match on the leaf.
			const std::string leaf = std::filesystem::path(path_component).filename().string();

			const auto it = g_assets.find(leaf);
			if (it == g_assets.end())
			{
				// Not a leaf-keyed asset. .sjson is keyed on the whole path
				// instead, against the assembled result rather than the
				// component we were asked for.
				substitute_sjson_overlay(output);
				return;
			}

			// FS_MAX_PATH is 512 in The Forge, and the buffer is the engine's.
			// Refuse rather than overrun it.
			constexpr size_t fs_max_path = 512;
			if (it->second.size() + 1 > fs_max_path)
			{
				LOGF(WARNING, "plugins_data: path too long for the engine's buffer, skipping {}", it->second);
				return;
			}

			memcpy(output, it->second.c_str(), it->second.size() + 1);

			// Once per file, not once per call: the engine assembles the same
			// path repeatedly and this would otherwise flood the log.
			static std::set<std::string> reported;
			static std::mutex reported_mutex;
			{
				std::scoped_lock lock(reported_mutex);
				if (reported.insert(leaf).second)
				{
					LOGF(INFO, "plugins_data: serving {} from {}", leaf, it->second);
				}
			}
		}

		void enumerate_detour(void* resource_dir, const char* sub_directory, const char* extension, void* out)
		{
			g_enumerate_hook.call<void, void*, const char*, const char*, void*>(resource_dir, sub_directory, extension, out);

			if (sub_directory && extension)
			{
				sjson_overlay::mark_directory_enumerated(sjson_overlay::normalize_path(sub_directory), extension);
			}

			LOGF(DEBUG,
			     "fsGetFilesWithExtension(dir={}, sub=\"{}\", ext=\"{}\") -> {} entry(ies)",
			     resource_dir,
			     sub_directory ? sub_directory : "",
			     extension ? extension : "",
			     vector_count(out));
		}
	}

	namespace
	{
		void install_append_path_hook()
		{
			// The Forge is C, so this is an undecorated symbol rather than a
			// mangled C++ one - it is in the map under its plain name.
			const auto address = game_symbol("fsAppendPathComponent");
			if (!address)
			{
				LOG(WARNING) << "fsAppendPathComponent is not in the symbol map; plugins_data assets will not load.";
				return;
			}

			g_append_path_hook = safetyhook::create_inline(reinterpret_cast<void*>(address),
			                                              reinterpret_cast<void*>(&append_path_detour));

			if (!g_append_path_hook)
			{
				LOG(ERROR) << "Could not hook fsAppendPathComponent; plugins_data assets will not load.";
				return;
			}

			LOG(DEBUG) << "Hooked fsAppendPathComponent for plugins_data assets.";
		}
	}

	void scan_plugin_data_files()
	{
		const auto root = g_file_manager.get_project_folder("plugins_data").get_path();

		std::error_code ec;
		if (!std::filesystem::exists(root, ec))
		{
			return;
		}

		size_t registered = 0;

		auto options = std::filesystem::directory_options::skip_permission_denied
		    | std::filesystem::directory_options::follow_directory_symlink;

		for (std::filesystem::recursive_directory_iterator it(root, options, ec), end; it != end; it.increment(ec))
		{
			if (ec)
			{
				LOGF(WARNING, "plugins_data: {}", ec.message());
				ec.clear();
				continue;
			}

			// The SJSON overlay directory is indexed by logical path, not by
			// leaf, so it must not also be swept up here. Hades 1 ships
			// MainMenuScreen.sjson and InGameUI.sjson under two directories
			// each, and a leaf-keyed entry would serve whichever was found
			// first.
			if (it->is_directory(ec) && it->path().filename() == sjson_overlay::DATA_DIR_NAME)
			{
				it.disable_recursion_pending();
				continue;
			}

			if (!it->is_regular_file(ec) || !is_engine_asset(it->path().extension()))
			{
				continue;
			}

			// Registered under the bare filename. file_redirect matches a
			// trailing path component in either direction, so a mod does not
			// have to reproduce the engine's folder layout under plugins_data
			// to replace Win/Packages/Foo.pkg - dropping Foo.pkg anywhere works.
			// Materialised, not a pointer into a temporary: u8string() returns
			// by value, so .c_str() on it dangles the moment the statement ends.
			const std::u8string u8name = it->path().filename().u8string();
			const std::string filename(reinterpret_cast<const char*>(u8name.c_str()), u8name.size());

			g_assets[filename] = reinterpret_cast<const char*>(it->path().u8string().c_str());
			++registered;

			LOGF(DEBUG, "plugins_data: {} -> {}", filename, reinterpret_cast<const char*>(it->path().u8string().c_str()));
		}

		if (registered)
		{
			LOGF(INFO, "plugins_data: {} asset file(s) registered.", registered);
		}

		sjson_overlay::scan_all_plugin_data(root);

		if (!g_assets.empty() || sjson_overlay::any_registered())
		{
			install_append_path_hook();
		}
	}

	// Deliberately read-only, for now.
	//
	// Hell2Modding's version of this hook injects mod filenames straight into
	// the engine's result vector, which is how a Hades 2 mod ships a brand new
	// package rather than replacing an existing one. That step is not safe to
	// copy here yet: the vector is an
	// eastl::vector<eastl::basic_string<char, allocator_forge>, allocator_forge>,
	// and our EASTL has no allocator_forge - it would allocate the pushed
	// string on our heap for The Forge to free on its own. Same class of
	// mistake as freeing lovely's HeapAlloc'd buffer with free(), and much
	// harder to see when it goes wrong.
	//
	// Replacement of files the engine already knows about does not need any of
	// that, and is what scan_plugin_data_files above already provides through
	// PlatformOpenFile. The probe is here to establish what Hades 1 actually
	// enumerates - whether it discovers packages this way at all - before
	// anyone writes the injection half against a guess.
	bool install_asset_enumeration_probe()
	{
		const auto address = game_symbol("fsGetFilesWithExtension");
		if (!address)
		{
			LOG(DEBUG) << "fsGetFilesWithExtension is not in the symbol map; asset enumeration will not be logged.";
			return false;
		}

		g_enumerate_hook = safetyhook::create_inline(reinterpret_cast<void*>(address),
		                                             reinterpret_cast<void*>(&enumerate_detour));

		if (!g_enumerate_hook)
		{
			LOG(WARNING) << "Could not hook fsGetFilesWithExtension.";
			return false;
		}

		LOG(DEBUG) << "Hooked fsGetFilesWithExtension (probe only).";
		return true;
	}
}
