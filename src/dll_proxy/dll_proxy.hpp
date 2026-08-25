#pragma once

namespace big
{
	struct dll_proxy
	{
		static void init();

		// The genuine d3d11.dll from System32. We are loaded *as* d3d11.dll, so
		// GetModuleHandleA("d3d11.dll") is ambiguous - the renderer needs this
		// to reach the real D3D11CreateDeviceAndSwapChain when building a
		// throwaway swap chain to read its vtable.
		static void* original_module();
	};
} // namespace big
