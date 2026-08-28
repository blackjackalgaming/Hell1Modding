#include "common.hpp"

#include "file_redirect.hpp"
#include "game_pdb.hpp"
#include "hades_lua.hpp"
#include "lua_extensions/bindings/paths_ext.hpp"

#include <safetyhook.hpp>

// Serving a mod's file in place of the game's, without touching the game's.
//
// This is what ModImporter's `Replace` directive means, and it is how every
// non-Lua asset gets into Hades: .pkg and .pkg_manifest for art, .thing_bin and
// .map_text for rooms, .bik for movies, .fsb for audio, .csv for subtitles.
// Goddess Codex alone Replaces 71 files.
//
// ModImporter did this by overwriting the game's files and keeping backups,
// because it was a launcher and had no other option. We are inside the process,
// so we answer the *open* instead: when the engine asks for
// Content/Win/Packages/OEHestia.pkg it is handed the mod's copy, and the file
// on disk is never modified. Uninstalling stays "delete the folder", Steam's
// file verification stays clean, and a crash cannot leave a half-written asset
// behind.
//
// The hook point is `PlatformOpenFile`, which is The Forge's own entry:
//
//   bool PlatformOpenFile(ResourceDirectory, const char* path, FileMode, FileStream*)
//   ?PlatformOpenFile@@YA_NW4ResourceDirectory@@PEBDW4FileMode@@PEAUFileStream@@@Z
//
// `sgg::PlatformFile::CreateStream` has the identical signature and sits above
// it, so hooking the lower one catches both. Hell2Modding hooks the same pair
// for its SJSON overlay; the difference here is that Hades 1 has no
// `CreateStreamWithRetry`, which is the third entry they also cover.

namespace big::hades1
{
	namespace
	{
		SafetyHookInline g_open_file_hook{};

		struct redirect
		{
			std::string target; // normalised: lower case, forward slashes
			std::string source; // absolute, native separators
			std::string owner;
		};

		std::vector<redirect> g_redirects;
		std::mutex g_mutex;

		std::string to_forward_slashes(std::string s)
		{
			std::replace(s.begin(), s.end(), '\\', '/');
			return s;
		}

		std::string normalise(std::string_view path)
		{
			std::string out;
			out.reserve(path.size());

			for (const char c : path)
			{
				out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}

			return out;
		}

		// True when `tail` is a path-boundary suffix of `whole`, so
		// "OEHestia.pkg" matches "win/packages/OEHestia.pkg" but not
		// "win/packages/NotOEHestia.pkg".
		bool ends_with_component(const std::string& whole, const std::string& tail)
		{
			if (whole.size() < tail.size())
			{
				return false;
			}

			const size_t at = whole.size() - tail.size();
			if (whole.compare(at, tail.size(), tail) != 0)
			{
				return false;
			}

			return at == 0 || whole[at - 1] == '/';
		}

		// The engine does not ask in the shape the modfile writes.
		//
		// Paths arrive relative to a ResourceDirectory, so the request can be
		// *shorter* than the target - the engine opens "VO.fsb" for what the
		// modfile calls "Audio/FMOD/Build/Desktop/VO.fsb" - or longer, when it
		// carries a Content/ prefix. So either may be the suffix of the other,
		// and the longest overlap wins: that is what keeps
		// "Movies/720p/MainMenuIn.bik" and "Movies/MainMenuIn.bik" apart when
		// a mod replaces both, which Goddess Codex does.
		const redirect* find_redirect(const char* path)
		{
			if (!path || !*path)
			{
				return nullptr;
			}

			const std::string wanted = normalise(path);

			std::scoped_lock lock(g_mutex);

			const redirect* best = nullptr;
			size_t best_overlap  = 0;

			for (const auto& entry : g_redirects)
			{
				const bool match = ends_with_component(wanted, entry.target) || ends_with_component(entry.target, wanted);
				if (!match)
				{
					continue;
				}

				const size_t overlap = std::min(wanted.size(), entry.target.size());
				if (overlap > best_overlap)
				{
					best_overlap = overlap;
					best         = &entry;
				}
			}

			return best;
		}

		// Express `source` relative to whatever directory the engine's request
		// was relative to.
		//
		// Each ResourceDirectory has a base the loader never sees, and passing
		// RD_ROOT with a fully qualified path simply fails - measured. But the
		// base is derivable: the engine asked for "VO.fsb" for what the modfile
		// calls "Audio/FMOD/Build/Desktop/VO.fsb", so the part of the target
		// the request does not account for is exactly that directory. Rebasing
		// against it yields a path the engine resolves with the RD it already
		// chose, which is the same trick ScriptManager::Load relies on for
		// "../Mods/...".
		std::string rebase_for_resource_dir(const std::string& target, const std::string& wanted, const std::string& source)
		{
			static const auto content = lua::paths_ext::get_game_executable_folder().parent_path() / "Content";

			std::error_code ec;

			// The engine gave a path at least as long as the modfile's, so the
			// request is already Content-relative or better.
			const auto base = (wanted.size() >= target.size()) ? content : content / target.substr(0, target.size() - wanted.size());

			const auto rel = std::filesystem::relative(source, base, ec);
			if (ec || rel.empty())
			{
				return {};
			}

			return to_forward_slashes(rel.string());
		}

		bool open_file_detour(int resource_directory, const char* path, int mode, void* stream)
		{
			if (const redirect* entry = find_redirect(path))
			{
				// The Forge's FileMode is a bit field, not an enum of one
				// value: a normal asset read arrives as FM_READ|FM_BINARY = 9.
				// Only reads are substituted, so a write can never land on the
				// mod's copy of a file.
				constexpr int fm_read  = 1 << 0;
				constexpr int fm_write = 1 << 1;

				if ((mode & fm_read) && !(mode & fm_write))
				{
					static thread_local bool reentering = false;
					if (!reentering)
					{
						reentering = true;

						// Reuse the engine's own ResourceDirectory. Passing
						// RD_ROOT with an absolute path was tried first and
						// fails - every open reported "could not open its
						// replacement" - because the RD is not a hint the
						// engine can ignore.
						const std::string rebased =
						    rebase_for_resource_dir(entry->target, normalise(path), entry->source);

						const bool ok = !rebased.empty()
						    && g_open_file_hook.call<bool, int, const char*, int, void*>(resource_directory,
						                                                                rebased.c_str(),
						                                                                mode,
						                                                                stream);

						reentering = false;

						if (ok)
						{
							LOGF(DEBUG, "Redirected {} -> {} ({})", path, entry->source, entry->owner);
							return true;
						}

						LOGF(ERROR,
						     "Content/Mods: {} could not open its replacement for \"{}\"; falling back to the game's "
						     "own file.",
						     entry->owner,
						     path);
					}
				}
			}

			return g_open_file_hook.call<bool, int, const char*, int, void*>(resource_directory, path, mode, stream);
		}
	}

	void add_file_redirect(const std::string& target, const std::filesystem::path& source, const std::string& owner)
	{
		std::scoped_lock lock(g_mutex);

		const std::string key = normalise(target);

		// Later registrations win, matching the Load Priority order the caller
		// registers in - the last mod to claim a file is the one that gets it.
		for (auto& entry : g_redirects)
		{
			if (entry.target == key)
			{
				LOGF(WARNING,
				     "Content/Mods: {} and {} both replace \"{}\"; {} wins.",
				     entry.owner,
				     owner,
				     target,
				     owner);

				entry.source = source.string();
				entry.owner  = owner;
				return;
			}
		}

		g_redirects.push_back({key, source.string(), owner});
	}

	void clear_file_redirects()
	{
		std::scoped_lock lock(g_mutex);
		g_redirects.clear();
	}

	bool install_file_redirect_hook()
	{
		const auto address = game_symbol("PlatformOpenFile");
		if (!address)
		{
			LOG(WARNING) << "PlatformOpenFile not in the symbol map; Content/Mods file replacement is unavailable.";
			return false;
		}

		g_open_file_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), open_file_detour);
		LOG(DEBUG) << (g_open_file_hook ? "Hooked PlatformOpenFile for Content/Mods file replacement."
		                                : "safetyhook refused PlatformOpenFile.");

		return static_cast<bool>(g_open_file_hook);
	}
}
