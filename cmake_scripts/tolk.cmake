# Tolk - a thin abstraction over the Windows screen reader drivers (NVDA, JAWS,
# Window-Eyes, SAPI and friends). Mods use it to speak UI text aloud.
#
# xiaoxiao921's fork is the same one Hell2Modding builds; it exists to add a
# CMake build to the original, which shipped only a Visual Studio solution.

include(FetchContent)

FetchContent_Declare(
	tolk
	GIT_REPOSITORY https://github.com/xiaoxiao921/tolk.git
	GIT_TAG 91e9aec1b6dfe68e5995153e8246e1b37d25870b
)
FetchContent_MakeAvailable(tolk)
