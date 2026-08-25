#include "common.hpp"

#include "audio.hpp"
#include "hades1/hades_lua.hpp"

#include <safetyhook.hpp>

// Custom FMOD bank loading, reached through FMOD Studio's public API rather
// than Supergiant's wrapper.
//
// Hell2Modding drives sgg::AudioManager::LoadBank, but that does not port:
// Hades 1's is a *private instance* method taking
// (bool, const eastl::string&, eastl::vector<BankInfo>&, const eastl::string&)
// where theirs takes (eastl::string_view*, PackageGroup). Half their audio.cpp
// is also a byte-pattern scan of Hades 2's compiled machine code.
//
// None of that is necessary. EngineWin64s.dll imports FMOD Studio directly:
//
//   ?loadBankFile@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVBank@23@@Z
//     FMOD_RESULT System::loadBankFile(const char*, unsigned int, Bank**)
//
// That is the documented public API, stable across FMOD versions, and
// fmodstudio.dll exports it - so GetProcAddress reaches it with no PDB symbols
// and no signature guesswork.
//
// The one thing not exposed statically is the System instance; the engine keeps
// it as a member of AudioManager. Rather than chase a struct offset, hook
// System::update, whose `this` *is* the System. It runs every frame, so there
// is no race against audio startup.

namespace
{
	// Mangled exports of fmodstudio.dll, as imported by EngineWin64s.dll.
	constexpr auto FMOD_STUDIO_SYSTEM_UPDATE       = "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ";
	constexpr auto FMOD_STUDIO_SYSTEM_LOADBANKFILE = "?loadBankFile@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVBank@23@@Z";

	constexpr unsigned int FMOD_STUDIO_LOAD_BANK_NORMAL = 0;
	constexpr int FMOD_OK                               = 0;

	SafetyHookInline g_update_hook{};

	// Note this is an FMOD *handle*, not a dereferenceable pointer - it encodes
	// an index and generation, so it looks bogus (0x1FFF1F) and VirtualQuery
	// reports it unmapped. That is normal; pass it straight back to FMOD.
	std::atomic<void*> g_studio_system{nullptr};

	int update_detour(void* studio_system)
	{
		if (!g_studio_system.load(std::memory_order_relaxed))
		{
			g_studio_system.store(studio_system, std::memory_order_relaxed);
			LOGF(INFO, "FMOD Studio System captured at {}.", studio_system);
		}

		return g_update_hook.call<int, void*>(studio_system);
	}

	// Lua API: Function
	// Table: audio
	// Name: load_bank
	// Param: file_path: string: absolute path to an FMOD .bank file
	// Returns: bool: true if FMOD accepted the bank.
	// Loads an FMOD Studio bank into the game's audio system. The game's own
	// banks live in Content/Audio/FMOD/Build/Desktop; a mod's can live
	// anywhere, since this takes a full path.
	bool load_bank(const std::string& file_path)
	{
		void* system = g_studio_system.load(std::memory_order_relaxed);
		if (!system)
		{
			LOG(ERROR) << "audio.load_bank: FMOD Studio System not captured yet (called before audio start-up?).";
			return false;
		}

		const HMODULE fmodstudio = GetModuleHandleA("fmodstudio.dll");
		if (!fmodstudio)
		{
			LOG(ERROR) << "audio.load_bank: fmodstudio.dll is not loaded.";
			return false;
		}

		// FMOD_RESULT (member) => this in RCX, so a plain function pointer with
		// the instance as the first argument matches the x64 ABI.
		using load_bank_file_t     = int (*)(void*, const char*, unsigned int, void**);
		const auto load_bank_file  = reinterpret_cast<load_bank_file_t>(
		    GetProcAddress(fmodstudio, FMOD_STUDIO_SYSTEM_LOADBANKFILE));

		if (!load_bank_file)
		{
			LOG(ERROR) << "audio.load_bank: loadBankFile not exported by fmodstudio.dll.";
			return false;
		}

		if (!std::filesystem::exists(file_path))
		{
			LOGF(ERROR, "audio.load_bank: no such file: {}", file_path);
			return false;
		}

		void* bank             = nullptr;
		const int fmod_result  = load_bank_file(system, file_path.c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);

		if (fmod_result != FMOD_OK)
		{
			// The engine statically links FMOD's error-string helper, so we can
			// report what FMOD actually said instead of a bare number.
			const char* reason = "?";
			if (const auto err_str = big::hades1::game_symbol("FMOD_ErrorString"))
			{
				reason = reinterpret_cast<const char* (*)(int)>(err_str)(fmod_result);
			}
			LOGF(ERROR, "audio.load_bank: FMOD refused {} - {} (FMOD_RESULT {}).", file_path, reason, fmod_result);
			return false;
		}

		LOGF(INFO, "audio.load_bank: loaded {}", file_path);
		return true;
	}
} // namespace

namespace lua::hades::audio
{
	bool install_hooks()
	{
		const HMODULE fmodstudio = GetModuleHandleA("fmodstudio.dll");
		if (!fmodstudio)
		{
			LOG(WARNING) << "fmodstudio.dll not loaded; audio.load_bank unavailable.";
			return false;
		}

		const auto update_addr = GetProcAddress(fmodstudio, FMOD_STUDIO_SYSTEM_UPDATE);
		if (!update_addr)
		{
			LOG(WARNING) << "FMOD Studio System::update not exported; audio.load_bank unavailable.";
			return false;
		}

		g_update_hook = safetyhook::create_inline(reinterpret_cast<void*>(update_addr),
		                                          reinterpret_cast<void*>(&update_detour));

		if (!g_update_hook)
		{
			LOG(ERROR) << "safetyhook refused FMOD Studio System::update.";
			return false;
		}

		LOG(INFO) << "Hooked FMOD Studio System::update to capture the audio system.";
		return true;
	}

	// Lua API: Table
	// Name: audio
	void bind(sol::table& state)
	{
		auto ns = state.create_named("audio");
		ns.set_function("load_bank", load_bank);
	}
} // namespace lua::hades::audio
