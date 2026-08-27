#include "common.hpp"

#include "dll_proxy/dll_proxy.hpp"
#include "config/config.hpp"
#include "file_manager/file_manager.hpp"
#include "renderer.hpp"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <safetyhook.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	// IDXGISwapChain vtable slots. IUnknown(3) + IDXGIObject(4) +
	// IDXGIDeviceSubObject(1) = 8, so Present is 8 and ResizeBuffers is 13.
	constexpr size_t VTABLE_PRESENT        = 8;
	constexpr size_t VTABLE_RESIZE_BUFFERS = 13;

	SafetyHookInline g_present_hook{};
	SafetyHookInline g_resize_hook{};

	ID3D11Device* g_device                   = nullptr;
	ID3D11DeviceContext* g_context           = nullptr;
	ID3D11RenderTargetView* g_render_target  = nullptr;
	HWND g_window                            = nullptr;
	WNDPROC g_original_wndproc               = nullptr;
	bool g_imgui_ready                       = false;
	toml_v2::config_file::config_entry<int>* g_ui_scale_percent = nullptr;

	std::mutex g_callback_mutex;
	std::vector<std::function<void()>> g_draw_callbacks;

	void create_render_target(IDXGISwapChain* swap_chain)
	{
		ID3D11Texture2D* back_buffer = nullptr;
		if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) && back_buffer)
		{
			g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
			back_buffer->Release();
		}
	}

	void release_render_target()
	{
		if (g_render_target)
		{
			g_render_target->Release();
			g_render_target = nullptr;
		}
	}

	LRESULT CALLBACK wndproc_detour(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// ImGui gets first refusal on input so its widgets work, but the game
		// still receives everything - we deliberately do not swallow messages.
		// Blocking game input while the overlay is open needs the engine's own
		// input layer, which is a separate job.
		if (g_imgui_ready)
		{
			ImGui_ImplWin32_WndProcHandler(window, msg, wparam, lparam);
		}

		return CallWindowProc(g_original_wndproc, window, msg, wparam, lparam);
	}

	void init_imgui(IDXGISwapChain* swap_chain)
	{
		if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device)
		{
			LOG(ERROR) << "Renderer: could not get the D3D11 device from the swap chain.";
			return;
		}

		g_device->GetImmediateContext(&g_context);

		DXGI_SWAP_CHAIN_DESC desc{};
		swap_chain->GetDesc(&desc);
		g_window = desc.OutputWindow;

		create_render_target(swap_chain);

		ImGui::CreateContext();

		// Without this ImGui writes imgui.ini relative to the CWD, which for us
		// is x64/ - i.e. next to Hades.exe instead of inside ReturnOfModding.
		// `path` must outlive the context: ImGui stores the pointer, it does not
		// copy the string.
		auto file_path          = big::g_file_manager.get_project_file("./imgui.ini").get_path();
		static std::string path = file_path.lexically_normal().make_preferred().string();
		ImGui::GetIO().IniFilename = path.c_str();

		ImGui::StyleColorsDark();

		// ImGui's default font is 13px, which on a 4K display is unreadable.
		// Scale off the actual back-buffer height rather than the DPI: the game
		// renders at its own resolution regardless of Windows' DPI setting, so
		// a 3840x2160 back buffer needs 2x whether or not the desktop is
		// scaled. 1080p is the baseline.
		//
		// "UI Scale Percent" of 0 means auto; anything else overrides.
		float ui_scale = desc.BufferDesc.Height > 0 ? static_cast<float>(desc.BufferDesc.Height) / 1080.0f : 1.0f;

		if (g_ui_scale_percent && g_ui_scale_percent->get_value() > 0)
		{
			ui_scale = static_cast<float>(g_ui_scale_percent->get_value()) / 100.0f;
		}

		ui_scale = std::clamp(ui_scale, 1.0f, 4.0f);

		// Rebuild the font at the target size rather than using
		// io.FontGlobalScale, which just magnifies the 13px bitmap and looks
		// blurry.
		ImFontConfig font_config{};
		font_config.SizePixels = 13.0f * ui_scale;
		ImGui::GetIO().Fonts->AddFontDefault(&font_config);

		// Padding, scrollbars and borders need to grow with it or the window
		// looks cramped around the larger text.
		ImGui::GetStyle().ScaleAllSizes(ui_scale);

		LOGF(DEBUG, "Renderer: UI scale {:.2f} for a {}x{} back buffer.", ui_scale, desc.BufferDesc.Width, desc.BufferDesc.Height);

		ImGui_ImplWin32_Init(g_window);
		ImGui_ImplDX11_Init(g_device, g_context);

		g_original_wndproc = WNDPROC(SetWindowLongPtrW(g_window, GWLP_WNDPROC, LONG_PTR(&wndproc_detour)));

		g_imgui_ready = true;

		LOGF(DEBUG, "Renderer: ImGui up on D3D11, window {}, ini at {}", static_cast<void*>(g_window), path);
	}

	// Shared by both present paths.
	void draw_overlay(IDXGISwapChain* swap_chain)
	{
		static std::atomic<int> calls{0};
		if (calls.fetch_add(1) == 0)
		{
			LOGF(DEBUG, "Renderer: first present on swap chain {}", static_cast<void*>(swap_chain));
		}

		if (!g_imgui_ready)
		{
			init_imgui(swap_chain);
		}

		if (!g_imgui_ready || !g_render_target)
		{
			return;
		}

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			{
				std::scoped_lock lock(g_callback_mutex);
				for (const auto& cb : g_draw_callbacks)
				{
					cb();
				}
			}

			ImGui::Render();
			g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	HRESULT WINAPI present_detour(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags)
	{
		draw_overlay(swap_chain);
		return g_present_hook.call<HRESULT, IDXGISwapChain*, UINT, UINT>(swap_chain, sync_interval, flags);
	}

	HRESULT WINAPI resize_buffers_detour(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
	{
		// The render target holds a reference to the old back buffer; resizing
		// with it alive fails, so drop it and rebuild afterwards.
		release_render_target();

		const HRESULT result =
		    g_resize_hook.call<HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>(swap_chain, buffer_count, width, height, format, flags);

		if (g_imgui_ready)
		{
			create_render_target(swap_chain);
		}

		return result;
	}

	// Hooking Present on a *throwaway* swap chain's vtable does not work here.
	// The engine imports D3D11CreateDevice and CreateDXGIFactory1, so it builds
	// its swap chain through a factory - and dxgi hands back a different
	// implementation class than D3D11CreateDeviceAndSwapChain does, with a
	// different function behind slot 8. The hook installed cleanly and never
	// fired once.
	//
	// So hook the factory call the engine itself makes and capture the real
	// swap chain, then hook Present on *that* object's vtable. Every step is
	// public COM; nothing depends on a struct layout.
	constexpr size_t VTABLE_CREATE_SWAP_CHAIN           = 10; // IDXGIFactory
	constexpr size_t VTABLE_CREATE_SWAP_CHAIN_FOR_HWND  = 15; // IDXGIFactory2
	constexpr size_t VTABLE_PRESENT1                    = 22; // IDXGISwapChain1

	SafetyHookInline g_create_swapchain_hook{};
	SafetyHookInline g_create_swapchain_for_hwnd_hook{};
	SafetyHookInline g_present1_hook{};

	std::once_flag g_present_hook_once;

	HRESULT WINAPI present1_detour(IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
	{
		draw_overlay(swap_chain);
		return g_present1_hook.call<HRESULT, IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*>(swap_chain, sync_interval, flags, params);
	}

	void hook_present_on(IDXGISwapChain* swap_chain)
	{
		if (!swap_chain)
		{
			return;
		}

		std::call_once(g_present_hook_once,
		               [swap_chain]
		               {
			               void** vtable = *reinterpret_cast<void***>(swap_chain);

			               g_present_hook = safetyhook::create_inline(vtable[VTABLE_PRESENT],
			                                                          reinterpret_cast<void*>(&present_detour));
			               g_resize_hook = safetyhook::create_inline(vtable[VTABLE_RESIZE_BUFFERS],
			                                                         reinterpret_cast<void*>(&resize_buffers_detour));

			               // Flip-model swap chains present through Present1, which
			               // is a different vtable slot - hooking only Present leaves
			               // the detour never firing, silently.
			               IDXGISwapChain1* swap_chain1 = nullptr;
			               if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain1))) && swap_chain1)
			               {
				               void** vtable1 = *reinterpret_cast<void***>(swap_chain1);
				               g_present1_hook = safetyhook::create_inline(vtable1[VTABLE_PRESENT1],
				                                                          reinterpret_cast<void*>(&present1_detour));
				               swap_chain1->Release();
			               }

			               LOGF(DEBUG,
			                    "Renderer: captured swap chain {}; Present {}, Present1 {}.",
			                    static_cast<void*>(swap_chain),
			                    g_present_hook ? "hooked" : "no",
			                    g_present1_hook ? "hooked" : "no");
		               });
	}

	HRESULT WINAPI create_swapchain_detour(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
	{
		const HRESULT hr =
		    g_create_swapchain_hook.call<HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>(factory, device, desc, out);

		if (SUCCEEDED(hr) && out && *out)
		{
			hook_present_on(*out);
		}

		return hr;
	}

	HRESULT WINAPI create_swapchain_for_hwnd_detour(IDXGIFactory2* factory,
	                                                IUnknown* device,
	                                                HWND hwnd,
	                                                const DXGI_SWAP_CHAIN_DESC1* desc,
	                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
	                                                IDXGIOutput* restrict_to_output,
	                                                IDXGISwapChain1** out)
	{
		const HRESULT hr = g_create_swapchain_for_hwnd_hook.call<HRESULT,
		                                                         IDXGIFactory2*,
		                                                         IUnknown*,
		                                                         HWND,
		                                                         const DXGI_SWAP_CHAIN_DESC1*,
		                                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
		                                                         IDXGIOutput*,
		                                                         IDXGISwapChain1**>(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, out);

		if (SUCCEEDED(hr) && out && *out)
		{
			hook_present_on(*out);
		}

		return hr;
	}
} // namespace

namespace big
{
	void add_renderer_draw_callback(std::function<void()> callback)
	{
		std::scoped_lock lock(g_callback_mutex);
		g_draw_callbacks.emplace_back(std::move(callback));
	}

	bool init_renderer()
	{
		// A throwaway factory, purely to read its vtable. dxgi's factory
		// implementation is shared, so the CreateSwapChain entry here is the
		// same function the engine will call through its own factory.
		IDXGIFactory* factory = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
		{
			LOG(ERROR) << "Renderer: CreateDXGIFactory1 failed; no overlay.";
			return false;
		}

		void** factory_vtable = *reinterpret_cast<void***>(factory);

		g_create_swapchain_hook = safetyhook::create_inline(factory_vtable[VTABLE_CREATE_SWAP_CHAIN],
		                                                    reinterpret_cast<void*>(&create_swapchain_detour));

		// The Forge may take either path depending on what it queries for, so
		// cover both rather than guess.
		IDXGIFactory2* factory2 = nullptr;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2)
		{
			void** factory2_vtable = *reinterpret_cast<void***>(factory2);

			g_create_swapchain_for_hwnd_hook =
			    safetyhook::create_inline(factory2_vtable[VTABLE_CREATE_SWAP_CHAIN_FOR_HWND],
			                              reinterpret_cast<void*>(&create_swapchain_for_hwnd_detour));

			factory2->Release();
		}

		factory->Release();

		if (!g_create_swapchain_hook && !g_create_swapchain_for_hwnd_hook)
		{
			LOG(ERROR) << "Renderer: could not hook either swap-chain creation path; no overlay.";
			return false;
		}

		LOGF(DEBUG,
		     "Renderer: watching for the engine's swap chain (CreateSwapChain {}, CreateSwapChainForHwnd {}).",
		     g_create_swapchain_hook ? "hooked" : "no",
		     g_create_swapchain_for_hwnd_hook ? "hooked" : "no");
		return true;
	}
} // namespace big

