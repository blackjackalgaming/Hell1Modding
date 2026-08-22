#include "common.hpp"

#include "dll_proxy/dll_proxy.hpp"
#include "logger/exception_handler.hpp"

// Hell1Modding - mod loader for Hades 1.
//
// This file is deliberately minimal. Hell2Modding's main.cpp is ~3000 lines,
// but almost all of it is Hades-2-specific engine surgery (extending the
// engine's global string buffers via mid-function hooks and Capstone-driven
// instruction rewriting). None of that applies here.
//
// What a ReturnOfModding fork actually has to provide is this: an exported
// my_main(), which RoM's proxy DLL machinery calls once the DLL is loaded.

extern "C" __declspec(dllexport) void my_main()
{
    using namespace big;

    // The proxy can end up invoking this more than once. Guard it.
    static bool already_executed = false;
    if (already_executed)
    {
        return;
    }
    already_executed = true;

    DisableThreadLibraryCalls(g_hmodule);

    // UTF-8 for the C APIs only. Setting the global C++ locale would also
    // change stringstream formatting (thousands separators in numbers), which
    // we don't want.
    setlocale(LC_ALL, ".utf8");

    // Sets up the proxy: loads the real d3d11.dll and forwards exports to it.
    dll_proxy::init();

    // Honours the env var / command line switch that disables the loader,
    // so users can launch vanilla without deleting the DLL.
    if (!rom::is_rom_enabled())
    {
        return;
    }

    // (project name, game executable, lua namespace exposed to plugins)
    rom::init("Hell1Modding", "Hades.exe", "rom");

    // Leaked on purpose - this module is never unloaded.
    const auto exception_handling = new exception_handler(true, nullptr);

    LOG(INFO) << "Hell1Modding attached";

    // ---------------------------------------------------------------------
    // NEXT: everything Hades-1-specific goes below, in src/hades1/.
    //
    //   1. read_game_pdb()  - port from Hell2Modding. Hades 1 ships
    //                         EngineWin64s.pdb, so ScriptManager::Load and
    //                         friends can be resolved by name instead of by
    //                         hardcoded RVA.
    //   2. hook ScriptManager::Update (RVA 0x27A580) - the only safe place to
    //                         touch Lua; Load() has a thread guard.
    //   3. call ScriptManager::Load (RVA 0x27E4B0) per mod, saving and
    //                         restoring HAS_CRASHED (RVA 0x1A128AB) around
    //                         each one so a single broken mod doesn't poison
    //                         the rest.
    // ---------------------------------------------------------------------
}
