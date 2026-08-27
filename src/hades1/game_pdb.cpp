#include "common.hpp"

#include "game_pdb.hpp"
#include "file_manager/file_manager.hpp"
#include "paths/paths.hpp"
#include "pdb_symbol_map.hpp"

#include <PDB.h>
#include <PDB_DBIStream.h>
#include <PDB_ImageSectionStream.h>
#include <PDB_InfoStream.h>
#include <PDB_PublicSymbolStream.h>
#include <PDB_RawFile.h>

// Ported from hell2-reference/src/main.cpp:2468. Two things differ for Hades 1:
//
//   - The symbols live in EngineWin64s.dll, not in the executable. Hades2.pdb
//     matches Hades2.exe, so Hell2Modding rebases onto GetModuleHandleA(0);
//     doing that here would produce addresses off by the difference between the
//     exe base and the engine DLL base - the kind of bug that looks like it
//     works right up until something jumps into the middle of nowhere.
//   - That DLL is not necessarily mapped yet when we run, hence wait_for_module.
//
// Error handling is also stricter than the original, which on failure logs,
// closes the mapping, and then carries on using it anyway.

namespace
{
	// Copied from raw_pdb's ExampleMemoryMappedFile.cpp. It lives in raw_pdb's
	// Examples target, which builds an executable, so there is nothing to link
	// against - Hell2Modding inlines the same code for the same reason.
	// Windows-only, as is the rest of this project.
	struct mapped_file
	{
		HANDLE file         = INVALID_HANDLE_VALUE;
		HANDLE file_mapping = INVALID_HANDLE_VALUE;
		void* base_address  = nullptr;
		size_t len          = 0;
	};

	mapped_file map_file(const char* path)
	{
		mapped_file result{};

		HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return result;
		}

		HANDLE file_mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (!file_mapping)
		{
			CloseHandle(file);
			return result;
		}

		void* base_address = MapViewOfFile(file_mapping, FILE_MAP_READ, 0, 0, 0);
		if (!base_address)
		{
			CloseHandle(file_mapping);
			CloseHandle(file);
			return result;
		}

		BY_HANDLE_FILE_INFORMATION file_information{};
		if (!GetFileInformationByHandle(file, &file_information))
		{
			UnmapViewOfFile(base_address);
			CloseHandle(file_mapping);
			CloseHandle(file);
			return result;
		}

		result.file         = file;
		result.file_mapping = file_mapping;
		result.base_address = base_address;
		result.len          = (static_cast<size_t>(file_information.nFileSizeHigh) << 32) | file_information.nFileSizeLow;
		return result;
	}

	void unmap_file(mapped_file& handle)
	{
		if (handle.base_address)
		{
			UnmapViewOfFile(handle.base_address);
		}
		if (handle.file_mapping != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle.file_mapping);
		}
		if (handle.file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle.file);
		}
		handle = {};
	}

	// We run early, off DllMain, so EngineWin64s.dll may not be mapped yet.
	// Poll rather than assume.
	HMODULE wait_for_module(const char* module_name, std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (true)
		{
			if (HMODULE module = GetModuleHandleA(module_name))
			{
				return module;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return nullptr;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	// raw_pdb only defines IsError in its example code, where it printfs.
	// A game process has nowhere useful to print, and every call site below
	// already logs something specific, so this just reports the fact.
	bool is_error(PDB::ErrorCode code)
	{
		return code != PDB::ErrorCode::Success;
	}

	bool has_valid_dbi_streams(const PDB::RawFile& raw_pdb_file, const PDB::DBIStream& dbi_stream)
	{
		return !is_error(dbi_stream.HasValidSymbolRecordStream(raw_pdb_file))
		    && !is_error(dbi_stream.HasValidPublicSymbolStream(raw_pdb_file))
		    && !is_error(dbi_stream.HasValidGlobalSymbolStream(raw_pdb_file))
		    && !is_error(dbi_stream.HasValidSectionContributionStream(raw_pdb_file))
		    && !is_error(dbi_stream.HasValidImageSectionStream(raw_pdb_file));
	}
} // namespace

namespace big::hades1
{
	bool read_game_pdb()
	{
		constexpr auto module_name = "EngineWin64s.dll";
		constexpr auto pdb_name    = "EngineWin64s.pdb";

		const HMODULE engine_module = wait_for_module(module_name, std::chrono::seconds(30));
		if (!engine_module)
		{
			LOG(ERROR) << module_name << " never showed up; symbol map unavailable.";
			return false;
		}

		const auto engine_base = reinterpret_cast<uintptr_t>(engine_module);
		LOG(DEBUG) << module_name << " base " << HEX_TO_UPPER(engine_base);

		const auto pdb_path        = paths::get_main_module_folder() / pdb_name;
		const auto pdb_path_string = pdb_path.u8string();
		const auto pdb_path_str    = reinterpret_cast<const char*>(pdb_path_string.c_str());

		mapped_file pdb_file = map_file(pdb_path_str);
		if (!pdb_file.base_address)
		{
			LOG(ERROR) << "Cannot memory-map " << pdb_path_str;
			return false;
		}

		// Every failure from here on has to unmap before returning.
		struct scope_unmap
		{
			mapped_file& f;
			~scope_unmap()
			{
				unmap_file(f);
			}
		} unmap_guard{pdb_file};

		if (is_error(PDB::ValidateFile(pdb_file.base_address, pdb_file.len)))
		{
			LOG(ERROR) << pdb_name << " failed validation.";
			return false;
		}

		const PDB::RawFile raw_pdb_file = PDB::CreateRawFile(pdb_file.base_address);
		if (is_error(PDB::HasValidDBIStream(raw_pdb_file)))
		{
			LOG(ERROR) << pdb_name << " has no valid DBI stream.";
			return false;
		}

		const PDB::InfoStream info_stream(raw_pdb_file);
		if (info_stream.UsesDebugFastLink())
		{
			LOG(ERROR) << pdb_name << " was linked with /DEBUG:FASTLINK, which is unsupported.";
			return false;
		}

		// Logged so that a game patch swapping the PDB is obvious in the log.
		// CLAUDE.md records the GUID the hardcoded RVAs were taken from.
		const auto h = info_stream.GetHeader();
		LOGF(DEBUG,
		     "{} age {}, GUID {:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
		     pdb_name,
		     h->age,
		     h->guid.Data1,
		     h->guid.Data2,
		     h->guid.Data3,
		     h->guid.Data4[0],
		     h->guid.Data4[1],
		     h->guid.Data4[2],
		     h->guid.Data4[3],
		     h->guid.Data4[4],
		     h->guid.Data4[5],
		     h->guid.Data4[6],
		     h->guid.Data4[7]);

		const PDB::DBIStream dbi_stream = PDB::CreateDBIStream(raw_pdb_file);
		if (!has_valid_dbi_streams(raw_pdb_file, dbi_stream))
		{
			LOG(ERROR) << pdb_name << " is missing a DBI sub-stream we need.";
			return false;
		}

		// Converts section + offset into an RVA; needed by everything below.
		const PDB::ImageSectionStream image_section_stream = dbi_stream.CreateImageSectionStream(raw_pdb_file);
		const PDB::ModuleInfoStream module_info_stream     = dbi_stream.CreateModuleInfoStream(raw_pdb_file);
		const PDB::CoalescedMSFStream symbol_record_stream = dbi_stream.CreateSymbolRecordStream(raw_pdb_file);
		const PDB::GlobalSymbolStream global_symbol_stream = dbi_stream.CreateGlobalSymbolStream(raw_pdb_file);
		const PDB::PublicSymbolStream public_symbol_stream = dbi_stream.CreatePublicSymbolStream(raw_pdb_file);

		using k = PDB::CodeView::DBI::SymbolRecordKind;

		// Global symbols - the data ones. LUA_INTERFACE, GLOBAL_TABLE,
		// HAS_CRASHED and LOADED_SCRIPT_FILES all land here.
		for (const PDB::HashRecord& hash_record : global_symbol_stream.GetRecords())
		{
			const PDB::CodeView::DBI::Record* record = global_symbol_stream.GetRecord(symbol_record_stream, hash_record);

			const char* name = nullptr;
			uint32_t rva     = 0u;

			switch (record->header.kind)
			{
			case k::S_GDATA32:
				name = record->data.S_GDATA32.name;
				rva = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_GDATA32.section, record->data.S_GDATA32.offset);
				break;
			case k::S_GTHREAD32:
				name = record->data.S_GTHREAD32.name;
				rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_GTHREAD32.section,
				                                                     record->data.S_GTHREAD32.offset);
				break;
			case k::S_LDATA32:
				name = record->data.S_LDATA32.name;
				rva = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LDATA32.section, record->data.S_LDATA32.offset);
				break;
			case k::S_LTHREAD32:
				name = record->data.S_LTHREAD32.name;
				rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LTHREAD32.section,
				                                                     record->data.S_LTHREAD32.offset);
				break;
			default:
				break;
			}

			// Control-flow-guard symbols and friends have no usable RVA.
			if (rva == 0u || !name)
			{
				continue;
			}

			hades1_insert_symbol_to_map(name, engine_base + rva);
		}

		// Public symbols. Hell2Modding skips these, and for Hades 2 it can: its
		// Lua is built with the project, so lua_* lands in the module streams.
		// Hades 1 links a prebuilt Lua with no per-module debug info, so without
		// this pass the entire Lua C API is invisible.
		for (const PDB::HashRecord& hash_record : public_symbol_stream.GetRecords())
		{
			const PDB::CodeView::DBI::Record* record = public_symbol_stream.GetRecord(symbol_record_stream, hash_record);
			if (record->header.kind != k::S_PUB32)
			{
				continue;
			}

			const uint32_t rva =
			    image_section_stream.ConvertSectionOffsetToRVA(record->data.S_PUB32.section, record->data.S_PUB32.offset);
			if (rva == 0u)
			{
				continue;
			}

			hades1_insert_symbol_to_map(record->data.S_PUB32.name, engine_base + rva);
		}

		// Module symbols - the functions. ScriptManager::Load / Update /
		// InitLua come from here.
		for (const PDB::ModuleInfoStream::Module& module : module_info_stream.GetModules())
		{
			if (!module.HasSymbolStream())
			{
				continue;
			}

			const PDB::ModuleSymbolStream module_symbol_stream = module.CreateSymbolStream(raw_pdb_file);
			module_symbol_stream.ForEachSymbol(
			    [&image_section_stream, engine_base](const PDB::CodeView::DBI::Record* record)
			    {
				    const char* name = nullptr;
				    uint32_t rva     = 0u;

				    switch (record->header.kind)
				    {
				    case k::S_THUNK32:
					    if (record->data.S_THUNK32.thunk == PDB::CodeView::DBI::ThunkOrdinal::TrampolineIncremental)
					    {
						    name = "ILT";
						    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_THUNK32.section,
						                                                          record->data.S_THUNK32.offset);
					    }
					    break;
				    case k::S_TRAMPOLINE:
					    // Incremental linking thunks live in the linker module.
					    name = "ILT";
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_TRAMPOLINE.thunkSection,
					                                                          record->data.S_TRAMPOLINE.thunkOffset);
					    break;
				    case k::S_LPROC32:
					    name = record->data.S_LPROC32.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LPROC32.section,
					                                                          record->data.S_LPROC32.offset);
					    hades1_insert_symbol_to_map_code_size(name, record->data.S_LPROC32.codeSize);
					    break;
				    case k::S_GPROC32:
					    name = record->data.S_GPROC32.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_GPROC32.section,
					                                                          record->data.S_GPROC32.offset);
					    hades1_insert_symbol_to_map_code_size(name, record->data.S_GPROC32.codeSize);
					    break;
				    case k::S_LPROC32_ID:
					    name = record->data.S_LPROC32_ID.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LPROC32_ID.section,
					                                                          record->data.S_LPROC32_ID.offset);
					    hades1_insert_symbol_to_map_code_size(name, record->data.S_LPROC32_ID.codeSize);
					    break;
				    case k::S_GPROC32_ID:
					    name = record->data.S_GPROC32_ID.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_GPROC32_ID.section,
					                                                          record->data.S_GPROC32_ID.offset);
					    hades1_insert_symbol_to_map_code_size(name, record->data.S_GPROC32_ID.codeSize);
					    break;
				    case k::S_LDATA32:
					    name = record->data.S_LDATA32.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LDATA32.section,
					                                                          record->data.S_LDATA32.offset);
					    break;
				    case k::S_LTHREAD32:
					    name = record->data.S_LTHREAD32.name;
					    rva  = image_section_stream.ConvertSectionOffsetToRVA(record->data.S_LTHREAD32.section,
					                                                          record->data.S_LTHREAD32.offset);
					    break;
				    default:
					    // S_BLOCK32 / S_LABEL32 carry no name; S_REGREL32 needs
					    // a live register value. Nothing to record for any of them.
					    break;
				    }

				    if (rva == 0u || !name)
				    {
					    return;
				    }

				    hades1_insert_symbol_to_map(name, engine_base + rva);
			    });
		}

		LOGF(DEBUG,
		     "Symbol map: {} symbols, {} with code sizes.",
		     hades1_symbol_to_address.size(),
		     hades1_symbol_to_code_size.size());

		return true;
	}
} // namespace big::hades1

namespace big::hades1
{
	void log_symbols_containing(const char* needle)
	{
		const HMODULE engine_module = GetModuleHandleA("EngineWin64s.dll");
		const auto engine_base      = reinterpret_cast<uintptr_t>(engine_module);

		int hits = 0;
		for (const auto& [name, address] : hades1_symbol_to_address)
		{
			if (name.find(needle) == std::string::npos)
			{
				continue;
			}

			// RVA, not the absolute address, so it can be compared directly
			// against Ghidra and the CLAUDE.md table.
			LOGF(INFO, "  [{}] RVA {:#X}  {}", needle, static_cast<uintptr_t>(address) - engine_base, name);
			hits++;
		}

		if (!hits)
		{
			LOGF(WARNING, "  [{}] no match", needle);
		}
	}
} // namespace big::hades1

namespace big::hades1
{
	bool resolve_known_symbols()
	{
		const auto engine_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("EngineWin64s.dll"));

		// The RVAs are the ones read out of Ghidra and written down in
		// CLAUDE.md. They are no longer used to find anything - they are kept
		// only so that a game update that moves a symbol says so in the log
		// instead of silently drifting.
		struct entry
		{
			const char* name;
			uintptr_t documented_rva;
			uintptr_t* out;
		};

		const entry entries[] = {
		    {"sgg::ScriptManager::Load",                0x27E4B0,  &g_symbols.script_manager_load    },
		    {"sgg::ScriptManager::Update",              0x27A580,  &g_symbols.script_manager_update  },
		    {"sgg::ScriptManager::InitLua",             0x27B9C0,  &g_symbols.script_manager_init_lua},
		    {"sgg::ScriptManager::LUA_INTERFACE",       0x69A2A0,  &g_symbols.lua_interface          },
		    {"sgg::ScriptManager::GLOBAL_TABLE",        0x1CEB728, &g_symbols.global_table           },
		    {"sgg::ScriptManager::HAS_CRASHED",         0x1A128AB, &g_symbols.has_crashed            },
		    {"sgg::ScriptManager::LOADED_SCRIPT_FILES", 0x69A270,  &g_symbols.loaded_script_files    },
		};

		bool all_found = true;

		for (const auto& e : entries)
		{
			const auto it = hades1_symbol_to_address.find(e.name);
			if (it == hades1_symbol_to_address.end())
			{
				LOGF(ERROR, "Symbol [MISSING] {}", e.name);
				all_found = false;
				continue;
			}

			const auto address = static_cast<uintptr_t>(it->second);
			*e.out             = address;

			const auto rva = address - engine_base;
			if (rva == e.documented_rva)
			{
				LOGF(DEBUG, "Symbol [OK  ] {:#09X}  {}", rva, e.name);
			}
			else
			{
				LOGF(WARNING,
				     "Symbol [MOVED] {:#09X} (CLAUDE.md says {:#09X})  {} - game updated, update the table",
				     rva,
				     e.documented_rva,
				     e.name);
			}
		}

		return all_found;
	}
} // namespace big::hades1

namespace big::hades1
{
	void dump_symbols_to_file()
	{
		const auto engine_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("EngineWin64s.dll"));
		const auto path        = g_file_manager.get_project_file("./symbols.txt").get_path();

		std::ofstream ofs(path);
		if (!ofs)
		{
			LOG(ERROR) << "Could not open symbols.txt for writing.";
			return;
		}

		// Sorted so diffing two game versions is meaningful.
		std::vector<std::pair<std::string, uintptr_t>> sorted;
		sorted.reserve(hades1_symbol_to_address.size());
		for (const auto& [name, address] : hades1_symbol_to_address)
		{
			sorted.emplace_back(name, static_cast<uintptr_t>(address) - engine_base);
		}
		std::sort(sorted.begin(), sorted.end());

		for (const auto& [name, rva] : sorted)
		{
			ofs << std::hex << std::uppercase << rva << " " << name << "\n";
		}

		LOGF(DEBUG, "Wrote {} symbols to symbols.txt", sorted.size());
	}
} // namespace big::hades1
