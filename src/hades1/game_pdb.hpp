#pragma once
#include <cstdint>

namespace big::hades1
{
	// Parses EngineWin64s.pdb and fills hades1_symbol_to_address /
	// hades1_symbol_to_code_size with every symbol it can resolve an RVA for,
	// rebased onto the loaded EngineWin64s.dll.
	//
	// Returns false if the module or the PDB is missing, or the PDB is
	// unusable; the maps are then empty and callers must fall back to the
	// hardcoded RVAs recorded in CLAUDE.md.
	bool read_game_pdb();

	// Logs every symbol whose name contains needle, with its RVA relative to
	// EngineWin64s.dll so the values line up with the table in CLAUDE.md and
	// with what Ghidra shows. Discovery aid: the PDB's exact spelling of a
	// symbol is rarely what you would guess.
	void log_symbols_containing(const char* needle);

	// Writes every resolved symbol as "RVA name" to ReturnOfModding/symbols.txt.
	// Not called on the normal path - it costs ~2 MB a launch. Call it from a
	// debug build or a breakpoint when chasing a name.
	// One run then answers any "what is this called in the PDB" question
	// offline, instead of a game restart per guess.
	void dump_symbols_to_file();

	// Absolute addresses of everything the loader needs, resolved by name.
	// Zero means unresolved.
	struct known_symbols
	{
		uintptr_t script_manager_load        = 0;
		uintptr_t script_manager_update      = 0;
		uintptr_t script_manager_init_lua    = 0;
		uintptr_t lua_interface              = 0;
		uintptr_t global_table               = 0;
		uintptr_t has_crashed                = 0;
		uintptr_t loaded_script_files        = 0;
	};

	inline known_symbols g_symbols;

	// Fills g_symbols from the parsed PDB, checking each against the RVA that
	// was read out of Ghidra and recorded in CLAUDE.md. A mismatch means the
	// game was patched and the documented RVAs are now stale - the by-name
	// address is still correct and is what gets used. Returns false if any
	// symbol is missing entirely.
	bool resolve_known_symbols();
} // namespace big::hades1
