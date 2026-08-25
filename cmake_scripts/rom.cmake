include(FetchContent)

# Same Lua as Hell2Modding, and for the same reason.
#
# Hades 1 embeds stock Lua 5.2 (luaL_newstate decompiles as unmodified
# lauxlib), and it is tempting to conclude that upstream lua/lua is therefore
# ABI-compatible and can drive the game's lua_State directly. It is not, and it
# cannot: doing that panics Lua and aborts the process. See "The Lua ABI
# problem" in CLAUDE.md.
#
# The fork is not about the *game's* Lua being unusual. It is about *ours*
# coexisting with the game's copy in one process: luaO_nilobject and dummynode
# are sentinel addresses private to each statically linked Lua, and two copies
# sharing a lua_State disagree about what "absent" and "empty table" mean. The
# fork externalises those so they can be pointed at the game's.
#
# Named -hades2, but the patch concerns Lua-hosting-Lua, not Hades 2.
set(LUA_CUSTOM_REPO https://github.com/xiaoxiao921/lua-fork-hades2.git)

set(LUA_GIT_HASH e2f0a33c52c18516c61b6fedfd6b518c5f0fbdb5)

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
