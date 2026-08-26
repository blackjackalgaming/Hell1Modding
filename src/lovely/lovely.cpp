#include "common.hpp"

#include "lovely.hpp"

#include "file_manager/file_manager.hpp"

// lovely-lib's C FFI. Mirrors crates/lovely-lib/src/lib.rs at the pinned
// revision in cmake_scripts/lovely.cmake; the two enums are #[repr(i32)] and
// the result struct is #[repr(C)], so these are layout-compatible.
enum class lovely_patch_table_load_result : int32_t
{
	ok                     = 0,
	cannot_read_mod_dir    = 1,
	bad_dir_entry          = 2,
	cannot_read_lovely_dir = 3,
	strip_prefix_failed    = 4,
	missing_parent_dir     = 5,
	file_read_failed       = 6,
	parse_error            = 7,
};

enum class lovely_apply_buffer_patches_status : int32_t
{
	ok                                 = 0,
	chunk_name_invalid                 = 1,
	mod_dir_name_invalid               = 2,
	byte_buffer_invalid                = 3,
	no_free_needed_use_original_buffer = 4,
	dump_dir_creation_failed           = 5,
	dump_file_write_failed             = 6,
	dump_meta_write_failed             = 7,
	buffer_allocation_failed           = 8,
};

struct lovely_apply_buffer_patches_result
{
	char* data_ptr;
	size_t data_len;
	lovely_apply_buffer_patches_status status;
};

extern "C"
{
	lovely_patch_table_load_result lovely_init(const char* plugins_directory_path_ptr);
	const lovely_apply_buffer_patches_result* lovely_apply_buffer_patches(const char* buf_ptr, size_t size, const char* name_ptr, const char* plugins_directory_path_ptr);
}

namespace big::lovely
{
	namespace
	{
		// lovely takes the plugins folder as a UTF-8 C string on every call and
		// only borrows it, so it is cached once rather than rebuilt per script.
		//
		// Hell2Modding passes `get_project_folder("plugins").get_path().u8string().c_str()`
		// inline, which dangles: the temporary std::u8string dies at the end of
		// the full expression, before lovely reads it. It works there by luck.
		std::string g_plugins_path;

		bool g_initialised = false;

		const char* to_string(lovely_patch_table_load_result result)
		{
			switch (result)
			{
			case lovely_patch_table_load_result::ok: return "Ok";
			case lovely_patch_table_load_result::cannot_read_mod_dir: return "CannotReadModDir";
			case lovely_patch_table_load_result::bad_dir_entry: return "BadDirEntry";
			case lovely_patch_table_load_result::cannot_read_lovely_dir: return "CannotReadLovelyDir";
			case lovely_patch_table_load_result::strip_prefix_failed: return "StripPrefixFailed";
			case lovely_patch_table_load_result::missing_parent_dir: return "MissingParentDir";
			case lovely_patch_table_load_result::file_read_failed: return "FileReadFailed";
			case lovely_patch_table_load_result::parse_error: return "ParseError";
			}

			return "unknown";
		}

		const char* to_string(lovely_apply_buffer_patches_status status)
		{
			switch (status)
			{
			case lovely_apply_buffer_patches_status::ok: return "Ok";
			case lovely_apply_buffer_patches_status::chunk_name_invalid: return "ChunkNameInvalid";
			case lovely_apply_buffer_patches_status::mod_dir_name_invalid: return "ModDirNameInvalid";
			case lovely_apply_buffer_patches_status::byte_buffer_invalid: return "ByteBufferInvalid";
			case lovely_apply_buffer_patches_status::no_free_needed_use_original_buffer: return "NoFreeNeededUseOriginalBuffer";
			case lovely_apply_buffer_patches_status::dump_dir_creation_failed: return "DumpDirCreationFailed";
			case lovely_apply_buffer_patches_status::dump_file_write_failed: return "DumpFileWriteFailed";
			case lovely_apply_buffer_patches_status::dump_meta_write_failed: return "DumpMetaWriteFailed";
			case lovely_apply_buffer_patches_status::buffer_allocation_failed: return "BufferAllocationFailed";
			}

			return "unknown";
		}

		// lovely allocates both the patched text and the result struct on the
		// process heap - HeapAlloc(GetProcessHeap()) directly for the former,
		// Rust's System allocator (which is the same thing for these sizes) for
		// the latter. Release them the same way rather than through the CRT's
		// free(), which only happens to work because the UCRT's heap is the
		// process heap.
		void release(const void* pointer)
		{
			if (pointer)
			{
				HeapFree(GetProcessHeap(), 0, const_cast<void*>(pointer));
			}
		}
	}

	void init()
	{
		if (g_initialised)
		{
			return;
		}

		// get_project_folder creates the directory; lovely returns
		// CannotReadModDir if it is missing, so this ordering matters on a
		// first run with no mods installed.
		const auto plugins_folder = g_file_manager.get_project_folder("plugins").get_path().u8string();
		g_plugins_path.assign(plugins_folder.begin(), plugins_folder.end());

		const auto result = lovely_init(g_plugins_path.c_str());

		// lovely writes its own log next to ours, and it is far more
		// informative than this line: one entry per patch file loaded and per
		// patch applied, with the mod that owns it.
		if (result == lovely_patch_table_load_result::ok)
		{
			LOGF(INFO, "lovely initialised over {} - see lovely.log for per-patch detail.", g_plugins_path);
		}
		else
		{
			LOGF(WARNING, "lovely_init failed: {}. Mods shipping lovely patches will load but do nothing.", to_string(result));
		}

		// Set even on failure: the patch table is empty, apply is a harmless
		// pass-through, and retrying init would re-run the global log setup.
		g_initialised = true;
	}

	patched_buffer::patched_buffer(const char* buffer, size_t size, const char* chunk_name) :
	    m_data(buffer),
	    m_size(size)
	{
		if (!g_initialised || !buffer || !chunk_name)
		{
			return;
		}

		const auto* result = lovely_apply_buffer_patches(buffer, size, chunk_name, g_plugins_path.c_str());

		if (!result)
		{
			return;
		}

		m_result = result;

		switch (result->status)
		{
		case lovely_apply_buffer_patches_status::ok:
			// The only status that means a patch actually landed, and the only
			// one where data_ptr is a new allocation rather than the buffer we
			// passed in.
			m_data      = result->data_ptr;
			m_size      = result->data_len;
			m_owns_data = true;
			break;

		case lovely_apply_buffer_patches_status::no_free_needed_use_original_buffer:
			// The overwhelmingly common case - no patch targets this script.
			break;

		case lovely_apply_buffer_patches_status::byte_buffer_invalid:
			// Not text. A precompiled chunk is not something lovely can patch
			// and never was, so this is expected rather than a fault.
			break;

		default:
			// Every remaining status is a failure, and lovely leaves data_ptr
			// pointing at our original buffer for all of them, so compiling the
			// unpatched script is both the safe and the correct fallback.
			LOGF(WARNING, "lovely failed on \"{}\": {}", chunk_name, to_string(result->status));
			break;
		}
	}

	patched_buffer::~patched_buffer()
	{
		if (m_owns_data)
		{
			release(m_data);
		}

		release(m_result);
	}
}
