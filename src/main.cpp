#include "common.hpp"

#include "console_focus.hpp"
#include "config/config.hpp"
#include "dll_proxy/dll_proxy.hpp"
#include "file_manager/file_manager.hpp"
#include "hades1/config_options.hpp"
#include "gui/gui.hpp"
#include "gui/renderer.hpp"
#include "hades1/game_pdb.hpp"
#include "hades1/file_redirect.hpp"
#include "hades1/log_write.hpp"
#include "lua_extensions/bindings/hades/audio.hpp"
#include "hades1/script_hook.hpp"
#include "logger/exception_handler.hpp"
#include "lovely/lovely.hpp"
#include "version.hpp"
#include "paths/paths.hpp"

// Hell1Modding - mod loader for Hades 1.
//
// This file is deliberately minimal. Hell2Modding's main.cpp is ~3000 lines,
// but almost all of it is Hades-2-specific engine surgery (extending the
// engine's global string buffers via mid-function hooks and Capstone-driven
// instruction rewriting). None of that applies here.
//
// What a ReturnOfModding fork actually has to provide is this: an exported
// my_main(), which RoM's proxy DLL machinery calls once the DLL is loaded.

static DWORD WINAPI late_init(LPVOID)
{
    using namespace big;


    // Nothing below creates the ReturnOfModding folder by itself:
    // get_project_root_folder() is what calls create_directories, and
    // g_file_manager has to be pointed at the result before the logger can
    // resolve ./LogOutput.log underneath it.
    const std::filesystem::path root_folder = paths::get_project_root_folder();
    g_file_manager.init(root_folder);
    paths::init_dump_file_path();

    // Must precede the logger: logger::initialize() binds a "Console Enabled"
    // option off config::general, and that pointer is null until this runs.
    config::init_general();

    // Bound before the logger on purpose. toml_v2's bind() returns any entry
    // that already exists, so whoever binds first decides the default - and
    // RoM's logger would otherwise start the console at
    // "DEBUG, INFO, WARNING, ERROR", which means every symbol we resolve,
    // every hook we install and several hundred lines of engine chatter.
    //
    // DEBUG still reaches the log file, which keeps its own full default.
    // Anyone who has already chosen a value keeps it; this only changes what
    // a fresh install starts with.
    config::general->bind("Logging",
                          "Console LogLevels",
                          "INFO, WARNING, ERROR",
                          "Only displays the specified log levels in the console. DEBUG carries the loader's own "
                          "internals and the engine's own logging - add it when diagnosing a problem. The log file "
                          "keeps everything regardless.");
    hades1::init_config_option_entries();

    // The overlay. Failure here is not fatal - the game just renders as it
    // always did.
    if (init_renderer())
    {
        gui::init();
    }

    static auto logger_instance = std::make_unique<logger>(
        rom::g_project_name, g_file_manager.get_project_file("./LogOutput.log"));

    static struct logger_cleanup
    {
        ~logger_cleanup() { Logger::Destroy(); }
    } g_logger_cleanup;

    // Purposely leaked - this module is never unloaded.
    const auto exception_handling = new exception_handler(true, nullptr);

    LOGF(INFO, "Hell1Modding v{} ({} on {})", big::version::VERSION_NUMBER, big::version::GIT_SHA1, big::version::GIT_BRANCH);
    LOG(DEBUG) << "Root folder: " << reinterpret_cast<const char*>(root_folder.u8string().c_str());

    // Before install_script_hook: the patch table has to be loaded before the
    // luaL_loadbufferx hook that consumes it can fire. It scans the plugins
    // folder, so it also has to come after g_file_manager.init.
    lovely::init();

    // Addresses come from EngineWin64s.pdb by name, so a game update moves
    // them without breaking us. Takes about a second, which is why it is on
    // this thread and not the game's.
    if (hades1::read_game_pdb())
    {
        hades1::resolve_known_symbols();

        hades1::apply_config_options();
        hades1::install_script_hook();
        hades1::install_file_redirect_hook();
        hades1::install_log_write_hook();
        lua::hades::audio::install_hooks();
    }

    // Last, because it blocks until the game window exists. Everything above
    // must already be installed.
    hand_console_focus_back_to_game();

    return 0;
}

extern "C" __declspec(dllexport) void my_main()
{
    using namespace big;

    static bool already_executed = false;
    if (already_executed) return;
    already_executed = true;

    DisableThreadLibraryCalls(g_hmodule);
    setlocale(LC_ALL, ".utf8");

    // Must stay synchronous - Hades calls D3D11 exports moments after load,
    // and PA has to be populated before that happens.
    dll_proxy::init();

    if (!rom::is_rom_enabled()) return;

    // Three string assignments, no I/O. Safe under the loader lock, and
    // paths::init_dump_file_path() reads g_project_name, so it has to have
    // happened before the thread gets that far.
    rom::init("Hell1Modding", "Hades.exe", "rom");

    // Everything else off the loader lock.
    CreateThread(nullptr, 0, late_init, nullptr, 0, nullptr);

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

// Called by Windows when the DLL is loaded. Runs synchronously during the
// game's import resolution, which is what makes dll_proxy::init() finish
// before Hades can call any D3D11 export - the null PA crash was this
// function not existing.
BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, PVOID)
{
    big::g_hmodule = hmod;
    my_main();
    return true;
}
