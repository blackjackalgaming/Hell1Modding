include(FetchContent)

# Hades 1 embeds stock Lua 5.2, statically compiled into EngineWin64s.dll.
#
# Turning LuaJIT off makes RoMBase build its Lua 5.2 path instead, which
# defaults to upstream lua/lua at 5.2.2 (see rom's cmake_scripts/lua.cmake).
# That should be ABI-compatible with the game's VM, so sol2 can drive the
# game's own lua_State directly rather than needing a second state and a bridge.
#
# Hell2Modding has to point LUA_CUSTOM_REPO at a custom fork because Hades 2's
# Lua differs from upstream. Hades 1's luaL_newstate decompiled as stock
# lauxlib, so we should be able to use upstream as-is.
#
# If the ABI turns out not to match exactly, the escape hatches are:
#   set(LUA_CUSTOM_REPO <fork url>)
#   set(LUA_GIT_HASH <commit>)
#   set(LUA_PATCH_PATH <path to .patch>)
set(LUA_USE_LUAJIT false)

add_compile_definitions(
    "IMGUI_USER_CONFIG=\"${SRC_DIR}/gui/imgui_config.hpp\""
)

# Pinned to the same commit Hell2Modding uses - a known-good revision of the
# base. Bump deliberately, never track a branch.
FetchContent_Declare(
        rom
        GIT_REPOSITORY https://github.com/xiaoxiao921/ReturnOfModdingBase.git
        GIT_TAG 87a45bd4c3d8d5fdffb3583b4172430fa5d16a1a
)
FetchContent_MakeAvailable(rom)