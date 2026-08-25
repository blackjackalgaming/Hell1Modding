#pragma once

namespace big::hades1
{
	// Mirrors the engine's own log output into the Hell1Modding console and
	// LogOutput.log. Without it, anything the engine reports - script errors
	// included - is invisible to us, because Hades 1 writes it elsewhere.
	//
	// Ported from hell2-reference/src/hades2/log_write.hpp. The symbol is
	// Log::Write, not log_write (which in Hades 1 belongs to GameAnalytics),
	// and the level argument is an unsigned int rather than a char:
	//
	//   ?Write@Log@@SAXIPEBDH0ZZ
	//   static void __cdecl Log::Write(unsigned int level, const char* file,
	//                                  int line, const char* msg, ...)
	//
	// Returns false if the symbol is missing or safetyhook refuses it.
	bool install_log_write_hook();
} // namespace big::hades1
