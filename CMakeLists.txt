cmake_minimum_required(VERSION 3.24)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Hell1Modding CXX C ASM ASM_MASM)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")

# Fix ImGui x86 compilation
add_compile_definitions(ImTextureID=ImU64)

# Fetch modules
message(STATUS "\nFetching modules")
include(cmake_scripts/capstone.cmake)
include(cmake_scripts/eastl.cmake)
include(cmake_scripts/git.cmake)
include(cmake_scripts/lovely.cmake)
include(cmake_scripts/lpeg.cmake)
include(cmake_scripts/luasocket.cmake)
include(cmake_scripts/raw_pdb.cmake)
include(cmake_scripts/rom.cmake)
include(cmake_scripts/safetyhook.cmake)
include(cmake_scripts/toml.cmake)


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

if (CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    source_group(
        TREE ${SRC_DIR}
        PREFIX "src"
        FILES ${SRC_MAIN}    
    )
endif()

message(STATUS "Hell1Modding")
#add_compile_options(/bigobj)
#add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
#add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
#add_executable(Hell1Modding WIN32 "${SRC_MAIN}")

add_library(Hell1Modding MODULE "${SRC_MAIN}")
target_compile_options(Hell1Modding
    PUBLIC
        $<$<OR:$<C_COMPILER_ID:MSVC>:/utf-8>,$<CXX_COMPILER_ID:MSVC>>:bigobj;/utf-8>
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
target_include_directories(Hell1Modding PRIVATE "${SRC_DIR}")
target_link_libraries(Hell1Modding 
    PRIVATE
        ReturnOfModdingBase
        dxgi
        dxguid
        Tolk
        lpeg_static
        wsock32
        ws2_32
        EASTL
        raw_pdb
        capstone
        safetyhook
        Zydis::Zydis
        ${LOVELY_LIB}
        userenv
        ntdll
)
# Warnings as errors
set_target_properties(Hell1Modding
   PROPERTIES
       COMPILE_WARNING_AS_ERROR ON
       OUTPUT_NAME "d3d11_"
)

if(MSVC AND FINAL)
    target_compile_options(Hell1Modding
        PUBLIC
            $<$<CONFIG:Release>:/Ob3> # Aggressive inline functions
            $<$<CONFIG:RelWithDebInfo>:/Ob3> # Aggressive inline functions
            /Oi # Enable intrinsic functions
            /Ot # Favor fast code
            /fp:fast # -ffast-math
            /GL # Whole program optimization
            /Gy # Function-level linking
            /GS- # Disable Buffer Security Check for optimization
            /Gw # Package global data in COMDAT sections for optimization
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
