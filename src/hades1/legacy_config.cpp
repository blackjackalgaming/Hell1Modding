#include "common.hpp"

#include "file_manager/file_manager.hpp"
#include "legacy_config.hpp"
#include "lua_extensions/lua_manager_extension.hpp"

// A Chalk equivalent for mods that have never heard of Chalk.
//
// Hades 2 mods opt into a config UI by calling `chalk.auto()`, which reads the
// mod's config.lua, writes config/<guid>.cfg, and merges the .cfg *over* the
// defaults so the user's edits win. Legacy Hades 1 mods ship the same shape of
// config.lua but cannot call anything - they predate all of it - so the loader
// has to do it on their behalf.
//
// The result is deliberately the same file Chalk would produce: config/<Mod>.cfg,
// a `config` root section, nested tables as dotted sub-sections, and the .cfg
// authoritative over the mod's own defaults.
//
// **The .cfg winning is free rather than merged.** toml_v2's config_file reads
// the existing file on construction and keeps every value it does not yet have
// an entry for; `bind` then hands back that stored value instead of the default
// it was passed. So binding the mod's default and reading the result back is
// already "file if present, default otherwise" - there is no merge step to get
// wrong, and no order dependence.

namespace big::hades1
{
	namespace
	{
		// Kept alive for the life of the process, not just long enough to write
		// the file: the overlay reads these every frame, and the game rebuilds
		// its Lua state twice a boot. A config_file holds only toml, no
		// registry refs, so it survives a wave boundary untouched - which also
		// means re-binding into the same one on the next wave is idempotent.
		std::vector<legacy_config_file> g_legacy_files;
		toml_v2::config_file* g_current = nullptr;
		std::set<std::string> g_globals_before;

		toml_v2::config_file* file_for(const std::string& mod_name, const std::filesystem::path& path)
		{
			for (auto& existing : g_legacy_files)
			{
				if (existing.mod_name == mod_name)
				{
					return existing.file.get();
				}
			}

			auto file = std::make_unique<toml_v2::config_file>(reinterpret_cast<const char*>(path.u8string().c_str()),
			                                                   true,
			                                                   mod_name);
			auto* raw = file.get();

			g_legacy_files.push_back({mod_name, std::move(file)});
			std::sort(g_legacy_files.begin(),
			          g_legacy_files.end(),
			          [](const auto& a, const auto& b)
			          {
				          return a.mod_name < b.mod_name;
			          });

			return raw;
		}

		// Every string key of _G, gathered by running `pairs` as a Lua chunk.
		//
		// This is not a workaround for anything on our side - it is the only
		// correct way to read _G once ModUtil is loaded. ModUtil replaces the
		// global table with a proxy: one real entry behind a metatable whose
		// __index and __pairs do the actual work. Raw traversal therefore reports
		// exactly one key, and it is right to. Measured, with the full mod stack
		// up:
		//
		//   _G has 1 key via our lua_next, 1 via the game's lua_next,
		//          1 via sol2, 3001 via pairs in the game's VM
		//
		// Note the game's own lua_next agrees at 1, which is what rules out the
		// two-Lua-copies explanation this comment used to give: it blamed our
		// luaH_next walking a game-allocated table, in the same class as the
		// luaO_nilobject and dummynode sentinels. That was wrong. Both copies
		// behave identically and both are obeying the language - only `pairs`
		// honours __pairs.
		//
		// Consequence for anything else that wants to sweep _G: doing it through
		// the C API will silently see one key. Run a chunk instead.
		std::set<std::string> global_keys(lua_State* L)
		{
			std::set<std::string> keys;

			sol::state_view state(L);

			auto result = state.safe_script("local t = {} "
			                                "for k in pairs(_G) do "
			                                "  if type(k) == 'string' then t[#t + 1] = k end "
			                                "end "
			                                "return table.concat(t, '\\n')",
			                                sol::script_pass_on_error);

			if (!result.valid())
			{
				LOG(WARNING) << "Could not enumerate globals: " << result.get<sol::error>().what();
				return keys;
			}

			const std::string joined = result.get<std::string>();

			size_t start = 0;
			while (start <= joined.size())
			{
				const size_t end = joined.find('\n', start);
				const auto name  = joined.substr(start, (end == std::string::npos ? joined.size() : end) - start);

				if (!name.empty())
				{
					keys.insert(name);
				}

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}

			return keys;
		}

		bool is_leaf(sol::object value)
		{
			const auto type = value.get_type();
			return type == sol::type::boolean || type == sol::type::number || type == sol::type::string;
		}

		// Nested tables become dotted *sections*, not dotted keys - Chalk's
		// merge() recurses with `section .. '.' .. k`. Getting this wrong
		// produces a file that looks right and is not interchangeable.
		void bind_table(sol::table table, const std::string& section, size_t depth, size_t& bound)
		{
			if (depth > 8)
			{
				return;
			}

			for (auto& [key_object, value] : table)
			{
				if (key_object.get_type() != sol::type::string)
				{
					// Array-like config is not something the .cfg format can
					// round-trip; leave it to the mod.
					continue;
				}

				const std::string key = key_object.as<std::string>();

				if (value.get_type() == sol::type::table)
				{
					bind_table(value.as<sol::table>(), section + "." + key, depth + 1, bound);
					continue;
				}

				if (!is_leaf(value))
				{
					continue;
				}

				// Bind the mod's default, then write back whatever the entry
				// actually holds: the .cfg's value when the file already had
				// one, the default when it did not.
				switch (value.get_type())
				{
				case sol::type::boolean:
				{
					auto* entry = g_current->bind(section, key, value.as<bool>(), "");
					table[key]  = entry->get_value();
					break;
				}
				case sol::type::number:
				{
					// Every Lua number is a double here, which is also what
					// Chalk records - its generated files say "Setting type:
					// double" even for whole numbers.
					auto* entry = g_current->bind(section, key, value.as<double>(), "");
					table[key]  = entry->get_value();
					break;
				}
				default:
				{
					auto* entry = g_current->bind(section, key, value.as<std::string>(), "");
					table[key]  = entry->get_value();
					break;
				}
				}

				++bound;
			}
		}

		// Legacy mods have no convention for where the table lives.
		//
		// A Hades 2 mod returns it, which is what Chalk relies on;
		// OverpoweredZagreusAspect assigns the global `OPZA` with a `Config`
		// field and returns nothing at all. So the only thing that works for
		// every case is to watch what config.lua added to _G.
		sol::table find_config_table(sol::state_view& state, const std::string& mod_name)
		{
			for (const auto& key : global_keys(state.lua_state()))
			{
				if (g_globals_before.contains(key))
				{
					continue;
				}

				sol::object value = state[key];
				if (value.get_type() != sol::type::table)
				{
					continue;
				}

				sol::table added = value.as<sol::table>();

				// `Mod = { Config = { ... } }` is the common shape; fall back
				// to the table itself for a mod that is flatter than that.
				sol::object nested = added["Config"];
				if (nested.get_type() == sol::type::table)
				{
					LOGF(DEBUG, "Content/Mods: {} config found at _G.{}.Config", mod_name, key);
					return nested.as<sol::table>();
				}

				LOGF(DEBUG, "Content/Mods: {} config found at _G.{}", mod_name, key);
				return added;
			}

			return sol::lua_nil;
		}
	}

	void snapshot_globals()
	{
		g_globals_before.clear();

		std::scoped_lock lock(lua_manager_extension::g_manager_mutex);
		if (!lua_manager_extension::is_lua_state_valid())
		{
			return;
		}

		g_globals_before = global_keys(lua_manager_extension::g_last_state);
	}

	void bind_legacy_config(const std::string& mod_name)
	{
		std::scoped_lock lock(lua_manager_extension::g_manager_mutex);
		if (!lua_manager_extension::is_lua_state_valid())
		{
			return;
		}

		sol::state_view state(lua_manager_extension::g_last_state);

		sol::table config = find_config_table(state, mod_name);
		if (!config.valid())
		{
			LOGF(DEBUG, "Content/Mods: {} imported a config.lua but added no global to read it from.", mod_name);
			return;
		}

		const auto path = g_file_manager.get_project_folder("config").get_path() / (mod_name + ".cfg");

		g_current = file_for(mod_name, path);

		size_t bound = 0;
		bind_table(config, "config", 0, bound);

		if (bound)
		{
			g_current->save();
			LOGF(INFO, "Content/Mods: {} config -> config/{}.cfg ({} setting(s))", mod_name, mod_name, bound);
		}

		g_current = nullptr;
	}

	const std::vector<legacy_config_file>& legacy_config_files()
	{
		return g_legacy_files;
	}
}
