cmake_minimum_required(VERSION 3.24)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Hell1Modding CXX C ASM ASM_MASM)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# Modules
#
# Dropped from Hell2Modding's list, and why:
#   tolk        - screen reader / accessibility. 0 of 78 mods use it.
#   capstone    - disassembler, used only for Hades-2-specific instruction
#                 rewriting we do not do. Genuinely not needed.
#
# Kept:
#   eastl       - Hades 1's engine uses EASTL throughout. Needed to build the
#                 eastl::string that ScriptManager::CallFunction wants, and to
#                 erase from LOADED_SCRIPT_FILES for hot reload.
#   raw_pdb     - parses PDB files. Hades 1 ships EngineWin64s.pdb, so this
#                 resolves addresses by name instead of hardcoding RVAs.
#   safetyhook  - function hooking (their choice over MinHook/PolyHook).
#   rom         - ReturnOfModdingBase itself.
#   git.cmake   - stamps GIT_SHA1 etc into version.cpp, and version.hpp holds
#                 VERSION_NUMBER, which the nightly CI seds. Needed to publish.
#   lpeg        - Lua pattern library, exposed to mods via require("lpeg").
#                 SGG_Modding-SJSON hard-requires it and 50 of 78 mods in a
#                 real profile depend on SJSON, so this is not optional.
#   luasocket   - socket/mime/http/ltn12 for mods. No mod in a 78-mod profile
#                 uses it today; added for API parity so a mod written against
#                 Hell2Modding does not hit a missing library.
#   lovely      - source-text patching of the game's own Lua scripts, applied
#                 in the luaL_loadbufferx hook. 6 of 78 mods in a real profile
#                 ship lovely patches, and without it those mods load, run,
#                 and silently do nothing. Needs a Rust toolchain.
# ---------------------------------------------------------------------------
message(STATUS "\nFetching modules")
include(cmake_scripts/git.cmake)
include(cmake_scripts/eastl.cmake)
include(cmake_scripts/lpeg.cmake)
include(cmake_scripts/luasocket.cmake)
include(cmake_scripts/lovely.cmake)
include(cmake_scripts/raw_pdb.cmake)
include(cmake_scripts/rom.cmake)
include(cmake_scripts/safetyhook.cmake)

file(GLOB_RECURSE SRC_MAIN
    "${SRC_DIR}/**.hpp"
    "${SRC_DIR}/**.h"
    "${SRC_DIR}/**.cpp"
    "${SRC_DIR}/**.cc"
    "${SRC_DIR}/**.cxx"
    "${SRC_DIR}/**.asm"
    "${SRC_DIR}/**.def"
    "${SRC_DIR}/**.rc"
)

if(CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    source_group(
        TREE ${SRC_DIR}
        PREFIX "src"
        FILES ${SRC_MAIN}
    )
endif()

message(STATUS "Hell1Modding")

# MODULE rather than SHARED: a plugin-style DLL, nothing links against it.
add_library(Hell1Modding MODULE "${SRC_MAIN}")

target_compile_options(Hell1Modding
    PUBLIC
        $<$<OR:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>:/bigobj;/utf-8>
)

target_compile_features(Hell1Modding PRIVATE cxx_std_23)

target_compile_definitions(Hell1Modding
    PUBLIC
        $<$<BOOL:${FINAL}>:FINAL>
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_precompile_headers(Hell1Modding PRIVATE "${SRC_DIR}/common.hpp")
target_include_directories(Hell1Modding PRIVATE ${SRC_DIR})

target_link_libraries(Hell1Modding
    PRIVATE
        ReturnOfModdingBase
        d3d11           # was d3d12 in Hell2Modding
        dxgi            # swapchain - needed by both D3D11 and D3D12
        dxguid
        EASTL
        lpeg_static
        ${LOVELY_LIB}
        luasocket_static
        raw_pdb
        safetyhook
        Zydis::Zydis
        userenv
        ntdll
)

set_target_properties(Hell1Modding
    PROPERTIES
        # Warnings-as-errors is ON in Hell2Modding. Leave it OFF until the
        # project builds, then turn it on and clean up.
        COMPILE_WARNING_AS_ERROR OFF
        OUTPUT_NAME "d3d11_"
)

if(MSVC AND FINAL)
    target_compile_options(Hell1Modding
        PUBLIC
            $<$<CONFIG:Release>:/Ob3>
            $<$<CONFIG:RelWithDebInfo>:/Ob3>
            /Oi
            /Ot
            /fp:fast
            /GL
            /Gy
            /GS-
            /Gw
    )
    target_link_options(Hell1Modding
        PUBLIC
            $<$<CONFIG:RelWithDebInfo>:
                /INCREMENTAL:NO
                /OPT:ICF,REF
                /LTCG
                /PDBALTPATH:d3d11.pdb
            >
    )
endif()