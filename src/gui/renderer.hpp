#pragma once

#include <functional>

namespace big
{
	// D3D11 present hook + ImGui backend.
	//
	// RoMBase ships no renderer at all - gui/ has only imgui_include.hpp and
	// widgets/ - because it is backend-specific. Hell2Modding's is driven off a
	// D3D12 present hook and is ~1000 lines; ours has to be D3D11, so the hook
	// and backend init below are new rather than ported. What *is* taken from
	// theirs is the init ordering and the imgui.ini redirect.
	//
	// Installs an inline hook on IDXGISwapChain::Present, found by creating a
	// throwaway swap chain and reading its vtable. Returns false if any of that
	// fails; the game then renders exactly as before.
	bool init_renderer();

	// Called inside Present, between ImGui::NewFrame and ImGui::Render.
	void add_renderer_draw_callback(std::function<void()> callback);
} // namespace big
