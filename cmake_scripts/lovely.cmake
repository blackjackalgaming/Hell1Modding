# lovely-lib - source-text patching of Lua scripts before they compile.
#
# A mod ships lovely/*.toml patch files describing edits to a target script by
# pattern, and lovely rewrites the buffer on its way into luaL_loadbufferx. It
# is how a mod changes the game's own scripts without shipping a whole copy of
# them, and 6 of 78 mods in a real profile depend on it.
#
# It is a Rust staticlib, so this is the one dependency that needs a toolchain
# beyond MSVC. Corrosion drives cargo from CMake and hands us a normal target.

include(FetchContent)

# A locally built lovely_lib wins, so a developer working on lovely itself does
# not have to fight FetchContent's pinned tag.
set(LOVELY_LIB_PATHS
    "${CMAKE_SOURCE_DIR}/lovely-lib/target/x86_64-pc-windows-msvc/release/lovely_lib.lib"
)

set(LOVELY_LIB "")

foreach(lib_path IN LISTS LOVELY_LIB_PATHS)
    if(EXISTS "${lib_path}")
        set(LOVELY_LIB "${lib_path}")
        message(STATUS "Found prebuilt lovely_lib at ${lib_path}")
        break()
    endif()
endforeach()

if(NOT LOVELY_LIB)
    find_program(RUSTUP_EXECUTABLE rustup)
    if(NOT RUSTUP_EXECUTABLE)
        find_program(RUSTC_EXECUTABLE rustc)
        if(NOT RUSTC_EXECUTABLE)
            message(FATAL_ERROR
                "lovely-lib needs a Rust toolchain and neither rustup nor rustc "
                "is on PATH. Install from https://rustup.rs and make sure the "
                "x86_64-pc-windows-msvc target is present.")
        endif()
    endif()

    # Corrosion integrates cargo into a CMake build. It also translates
    # CMAKE_MSVC_RUNTIME_LIBRARY into the matching +crt-static rustflag, which
    # matters here: we build /MT and a Rust staticlib defaults to /MD.
    FetchContent_Declare(
        corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.6
    )

    # Same pin Hell2Modding uses, so we inherit a revision known to work
    # against this API shape.
    FetchContent_Declare(
        lovely_lib
        GIT_REPOSITORY https://github.com/xiaoxiao921/lovely-lib.git
        GIT_TAG 19f5a98430bd09a6a0e337cd9e62d189b9c43f6a
    )
    FetchContent_MakeAvailable(corrosion lovely_lib)

    corrosion_import_crate(MANIFEST_PATH ${lovely_lib_SOURCE_DIR}/Cargo.toml)
    set(LOVELY_LIB "lovely_lib")
endif()
