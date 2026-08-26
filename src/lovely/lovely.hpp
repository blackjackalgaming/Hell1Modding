#pragma once

// lovely - source-text patching of the game's own Lua scripts.
//
// A mod ships plugins/<Mod>/lovely.toml (or plugins/<Mod>/lovely/*.toml)
// describing edits to a target script by pattern or regex. lovely rewrites the
// script's text on its way into luaL_loadbufferx, so the game compiles the
// patched version having never touched the file on disk - the same "change
// nothing on disk" principle this loader exists for, applied to the game's
// scripts rather than to its script list.
//
// This is the only dependency that is not C or C++: lovely-lib is a Rust
// staticlib, and everything below is its C FFI.

namespace big::lovely
{
	// Load the patch table from the plugins folder.
	//
	// Call exactly once. lovely installs a global tracing subscriber for its
	// own log file (ReturnOfModding/lovely.log) and installing one twice
	// panics inside Rust - which, across an FFI boundary, aborts the process
	// rather than raising anything catchable.
	void init();

	// One script's source text, run through the patch table.
	//
	// lovely hands back either the buffer we gave it (nothing matched, or it
	// failed) or a freshly allocated one it expects the caller to release.
	// This owns whichever it is, so the detour cannot leak or double-free.
	class patched_buffer
	{
	public:
		patched_buffer(const char* buffer, size_t size, const char* chunk_name);
		~patched_buffer();

		patched_buffer(const patched_buffer&)            = delete;
		patched_buffer& operator=(const patched_buffer&) = delete;

		const char* data() const
		{
			return m_data;
		}

		size_t size() const
		{
			return m_size;
		}

		// True only when the text actually changed, which is what makes a log
		// line worth writing - the overwhelming majority of scripts match no
		// patch at all.
		bool was_patched() const
		{
			return m_owns_data;
		}

	private:
		const void* m_result = nullptr;
		const char* m_data   = nullptr;
		size_t m_size        = 0;
		bool m_owns_data     = false;
	};
}
