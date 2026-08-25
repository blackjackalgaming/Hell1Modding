#pragma once

#include "memory/gm_address.hpp"

#include <string>
#include <unordered_map>

namespace big
{
	// Filled by read_game_pdb(). Keys are the decorated names as they appear in
	// EngineWin64s.pdb; values are already rebased onto the running module, so
	// they are absolute addresses, not RVAs.
	inline std::unordered_map<std::string, gmAddress> hades1_symbol_to_address;

	inline std::unordered_map<std::string, size_t> hades1_symbol_to_code_size;

	// A PDB happily contains the same name more than once - static functions
	// with the same name in different translation units, ILT thunks, and so on.
	// Rather than let later records overwrite earlier ones, the duplicates get
	// suffixed _2, _3, ... so the first occurrence keeps the bare name.
	template<typename T>
	inline void insert_unique(std::unordered_map<std::string, T>& map, const std::string& name, T value)
	{
		if (map.find(name) == map.end())
		{
			map[name] = value;
			return;
		}

		std::string new_name;
		int counter = 2;
		do
		{
			new_name = name + "_" + std::to_string(counter);
			counter++;
		} while (map.find(new_name) != map.end());

		map[new_name] = value;
	}

	inline void hades1_insert_symbol_to_map(const std::string& name, uintptr_t address)
	{
		insert_unique<gmAddress>(hades1_symbol_to_address, name, address);
	}

	inline void hades1_insert_symbol_to_map_code_size(const std::string& name, size_t code_size)
	{
		insert_unique<size_t>(hades1_symbol_to_code_size, name, code_size);
	}
} // namespace big
