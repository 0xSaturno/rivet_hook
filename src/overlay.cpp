// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <MinHook.h>

#include "bridge.hpp"

#include "overlay.hpp"

#include "scripting.hpp"

#include "runtime.hpp"
#include "signature.hpp"

extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rivet_hook {
	struct DescriptorHeapAllocator {
		ID3D12DescriptorHeap *Heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
		D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu {};
		D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu {};
		UINT HeapHandleIncrement = 0;
		ImVector<int> FreeIndices {};

		void
		Create(ID3D12Device *device, ID3D12DescriptorHeap *heap) {
			IM_ASSERT(Heap == nullptr && FreeIndices.empty());
			Heap = heap;
			const auto desc = heap->GetDesc();
			HeapType = desc.Type;
			HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
			HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
			HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
			FreeIndices.reserve(static_cast<int>(desc.NumDescriptors));
			for (int n = static_cast<int>(desc.NumDescriptors); n > 0; n--) {
				FreeIndices.push_back(n - 1);
			}
		}

		void
		Destroy() {
			Heap = nullptr;
			FreeIndices.clear();
		}

		void
		Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle) {
			IM_ASSERT(FreeIndices.Size > 0);
			const auto idx = FreeIndices.back();
			FreeIndices.pop_back();
			out_cpu_desc_handle->ptr = HeapStartCpu.ptr + idx * HeapHandleIncrement;
			out_gpu_desc_handle->ptr = HeapStartGpu.ptr + idx * HeapHandleIncrement;
		}

		void
		Free(const D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, const D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle) {
			const auto cpu_idx = static_cast<int>((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
			const auto gpu_idx = static_cast<int>((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
			IM_ASSERT(cpu_idx == gpu_idx);
			FreeIndices.push_back(cpu_idx);
		}
	};

	constexpr int MAX_FRAMES_IN_FLIGHT = 4;

	struct FrameContext {
		ID3D12CommandAllocator *CommandAllocator;
		UINT64 FenceValue;
	};

	static FrameContext g_frameContext[MAX_FRAMES_IN_FLIGHT] = {};
	static UINT g_frameIndex = 0;
	static int g_frameCount = 0;

	static ID3D12CommandQueue *g_pd3dCommandQueue = nullptr;
	static ID3D12DescriptorHeap *g_pd3dRtvDescHeap = nullptr;
	static ID3D12DescriptorHeap *g_pd3dSrvDescHeap = nullptr;
	static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
	static ID3D12GraphicsCommandList *g_pd3dCommandList = nullptr;
	static ID3D12Fence *g_fence = nullptr;
	static HANDLE g_fenceEvent = nullptr;
	static UINT64 g_fenceLastSignaledValue = 0;
	static ID3D12Resource *g_mainRenderTargetResource[MAX_FRAMES_IN_FLIGHT] = {};
	static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[MAX_FRAMES_IN_FLIGHT] = {};

	LRESULT APIENTRY
	WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	using execute_command_lists_t = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *pQueue, UINT NumCommandLists, ID3D12CommandList *ppCommandLists);
	execute_command_lists_t game_execute_command_lists = nullptr;
	constexpr int32_t D3D12_COMMAND_QUEUE_VTABLE_EXECUTE_COMMAND_LISTS = 10;

	using present_t = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags);
	present_t game_present = nullptr;
	constexpr int32_t DXGI_SWAP_CHAIN_VTABLE_PRESENT = 8;

	using resize_buffers_t = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	resize_buffers_t game_resize_buffers = nullptr;
	constexpr int32_t DXGI_SWAP_CHAIN_VTABLE_RESIZE_BUFFERS = 13;

	using destroy_swapchain_t = void* (*)(void* self);
	destroy_swapchain_t game_destroy_swapchain = nullptr;

	WNDPROC game_wnd_proc = nullptr;

	WNDCLASSEX window_class;
	HWND window_handle;

	bool bricked = false;
	bool imgui_initialized = false;
	bool imgui_visible = false;
	bool imgui_intercept_input = false;

	auto
	create_window() -> bool {
		memset(&window_class, 0, sizeof(window_class));
		window_class.cbSize = sizeof(WNDCLASSEX);
		window_class.style = CS_HREDRAW | CS_VREDRAW;
		window_class.lpfnWndProc = DefWindowProc;
		window_class.hInstance = GetModuleHandle(nullptr);
		window_class.lpszClassName = "rivet_hook dx window";
		RegisterClassEx(&window_class);

		window_handle = CreateWindow(window_class.lpszClassName, "DirectX Window", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, window_class.hInstance, nullptr);
		return window_handle != nullptr;
	}

	auto
	destroy_window() -> bool {
		DestroyWindow(window_handle);
		UnregisterClass(window_class.lpszClassName, window_class.hInstance);
		return window_handle == nullptr;
	}

	auto
	reset() -> void {
		if (g_pd3dCommandQueue && g_fence && g_fenceEvent) {
			g_fenceLastSignaledValue++;
			if (g_pd3dCommandQueue->Signal(g_fence, g_fenceLastSignaledValue) == S_OK) {
				if (g_fence->GetCompletedValue() < g_fenceLastSignaledValue) {
					if (g_fence->SetEventOnCompletion(g_fenceLastSignaledValue, g_fenceEvent) == S_OK) {
						WaitForSingleObject(g_fenceEvent, INFINITE);
					}
				}
			}
		}

		if (imgui_initialized) {
			imgui_initialized = false;
			ImGui_ImplDX12_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}

		g_pd3dSrvDescHeapAlloc.Destroy();
		for (auto &[command_allocator, _] : g_frameContext) {
			if (command_allocator) {
				command_allocator->Release();
				command_allocator = nullptr;
			}
		}
		memset(g_frameContext, 0, sizeof(g_frameContext));

		for (auto &resource : g_mainRenderTargetResource) {
			if (resource) {
				resource->Release();
				resource = nullptr;
			}
		}
		memset(g_mainRenderTargetDescriptor, 0, sizeof(g_mainRenderTargetDescriptor));

		if (g_pd3dCommandList) {
			g_pd3dCommandList->Release();
			g_pd3dCommandList = nullptr;
		}

		if (g_pd3dRtvDescHeap) {
			g_pd3dRtvDescHeap->Release();
			g_pd3dRtvDescHeap = nullptr;
		}

		if (g_pd3dSrvDescHeap) {
			g_pd3dSrvDescHeap->Release();
			g_pd3dSrvDescHeap = nullptr;
		}

		if (g_fence) {
			g_fence->Release();
			g_fence = nullptr;
		}

		if (g_fenceEvent) {
			CloseHandle(g_fenceEvent);
			g_fenceEvent = nullptr;
		}

		g_fenceLastSignaledValue = 0;
		g_frameIndex = 0;
		g_frameCount = 0;

		g_pd3dCommandQueue = nullptr;
	}

	auto STDMETHODCALLTYPE
	execute_command_lists(ID3D12CommandQueue *pQueue, const UINT NumCommandLists, ID3D12CommandList *ppCommandLists) -> void {
		if (!g_pd3dCommandQueue && pQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
			g_pd3dCommandQueue = pQueue;
		}

		game_execute_command_lists(pQueue, NumCommandLists, ppCommandLists);
	}

	auto STDMETHODCALLTYPE
	resize_buffers(IDXGISwapChain3 *pSwapChain, const UINT BufferCount, const UINT Width, const UINT Height, const DXGI_FORMAT NewFormat, const UINT SwapChainFlags) -> HRESULT {
		reset();
		return game_resize_buffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	}

	auto
	init_imgui(IDXGISwapChain3 *pSwapChain) -> bool {
		ID3D12Device *pd3dDevice;

		if (pSwapChain->GetDevice(IID_PPV_ARGS(&pd3dDevice)) != S_OK) {
			return false;
		}

		DXGI_SWAP_CHAIN_DESC desc;
		if (pSwapChain->GetDesc(&desc) != S_OK) {
			return false;
		}

		window_handle = desc.OutputWindow;
		if (!game_wnd_proc) {
			game_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(window_handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
		}

		const auto frame_count = desc.BufferCount;
		if (frame_count > MAX_FRAMES_IN_FLIGHT) {
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC rtv_descriptor = {};
		rtv_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtv_descriptor.NumDescriptors = frame_count;
		rtv_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		rtv_descriptor.NodeMask = 1;
		if (pd3dDevice->CreateDescriptorHeap(&rtv_descriptor, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK) {
			return false;
		}

		const auto rtvDescriptorSize = pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < frame_count; i++) {
			g_mainRenderTargetDescriptor[i] = rtvHandle;
			if (pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_mainRenderTargetResource[i])) != S_OK) {
				return false;
			}
			pd3dDevice->CreateRenderTargetView(g_mainRenderTargetResource[i], nullptr, rtvHandle);
			rtvHandle.ptr += rtvDescriptorSize;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srv_descriptor = {};
		srv_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srv_descriptor.NumDescriptors = frame_count;
		srv_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (pd3dDevice->CreateDescriptorHeap(&srv_descriptor, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK) {
			return false;
		}
		g_pd3dSrvDescHeapAlloc.Create(pd3dDevice, g_pd3dSrvDescHeap);

		for (UINT i = 0; i < frame_count; i++) {
			if (pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK) {
				return false;
			}
		}

		g_frameCount = static_cast<int>(frame_count);

		if (pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
			g_pd3dCommandList->Close() != S_OK) {
			return false;
		}

		if (pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK) {
			return false;
		}

		g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (g_fenceEvent == nullptr) {
			return false;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		ImGui::StyleColorsDark();
		auto& style = ImGui::GetStyle();
		style.Colors[ImGuiCol_WindowBg].w = 0.33f;
		style.Colors[ImGuiCol_TitleBg].w = 0.33f;

		ImGui_ImplWin32_Init(window_handle);

		ImGui_ImplDX12_InitInfo init_info = {};
		init_info.Device = pd3dDevice;
		init_info.CommandQueue = g_pd3dCommandQueue;
		init_info.NumFramesInFlight = g_frameCount;
		init_info.RTVFormat = desc.BufferDesc.Format;
		init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		init_info.SrvDescriptorHeap = g_pd3dSrvDescHeap;
		init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
			return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle);
		};
		init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *, const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
			return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle);
		};

		ImGui_ImplDX12_Init(&init_info);

		return true;
	}

	FrameContext *
	WaitForNextFrameContext(const UINT index) {
		FrameContext *frame_context = &g_frameContext[index % g_frameCount];
		if (g_fence && g_fence->GetCompletedValue() < frame_context->FenceValue) {
			if (g_fence->SetEventOnCompletion(frame_context->FenceValue, g_fenceEvent) == S_OK) {
				WaitForSingleObject(g_fenceEvent, INFINITE);
			}
		}

		return frame_context;
	}

	auto
	ToggleCursor() -> void {
		static RECT prevClip {};
		static bool hiddenByUs = false;

		CURSORINFO ci;
		ci.cbSize = sizeof(ci);
		if (!GetCursorInfo(&ci)) {
			return;
		}

		const auto isCurrentlyShowing = (ci.flags & CURSOR_SHOWING) != 0;

		if (imgui_intercept_input) {
			if (isCurrentlyShowing) {
				return;
			}

			SetCursor(LoadCursorA(nullptr, IDC_ARROW));
			GetClipCursor(&prevClip);
			ClipCursor(nullptr);

			while (ShowCursor(true) < 0) { }

			hiddenByUs = true;
		} else {
			if (!isCurrentlyShowing || !hiddenByUs) {
				return;
			}

			SetCursor(nullptr);
			ClipCursor(&prevClip);

			while (ShowCursor(false) >= 0) { }

			hiddenByUs = false;
		}
	}

	auto STDMETHODCALLTYPE
	present(IDXGISwapChain3 *pSwapChain, const UINT SyncInterval, const UINT flags) -> HRESULT {
		// the only point in the frame where walking engine state is safe
		bridge::pump();
		scripting::pump();

		if (g_pd3dCommandQueue == nullptr || bricked) {
			return game_present(pSwapChain, SyncInterval, flags);
		}

		if (!imgui_initialized) {
			if (!init_imgui(pSwapChain)) {
				bricked = true;
				reset();
				return game_present(pSwapChain, SyncInterval, flags);
			}

			imgui_initialized = true;
		}

		if (!imgui_visible) {
			return game_present(pSwapChain, SyncInterval, flags);
		}

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		Overlay::DrawImGUI();
		ImGui::Render();

		const UINT backBufferIdx = pSwapChain->GetCurrentBackBufferIndex();
		FrameContext *frameCtx = WaitForNextFrameContext(backBufferIdx);
		if (frameCtx->CommandAllocator->Reset() != S_OK) {
			return game_present(pSwapChain, SyncInterval, flags);
		}

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		if (g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr) != S_OK) {
			return game_present(pSwapChain, SyncInterval, flags);
		}
		g_pd3dCommandList->ResourceBarrier(1, &barrier);
		g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
		g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		g_pd3dCommandList->ResourceBarrier(1, &barrier);
		if (g_pd3dCommandList->Close() != S_OK) {
			return game_present(pSwapChain, SyncInterval, flags);
		}

		g_pd3dCommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&g_pd3dCommandList));
		if (g_pd3dCommandQueue->Signal(g_fence, ++g_fenceLastSignaledValue) != S_OK) {
			return game_present(pSwapChain, SyncInterval, flags);
		}
		frameCtx->FenceValue = g_fenceLastSignaledValue;

		g_frameIndex++;
		return game_present(pSwapChain, SyncInterval, flags);
	}

	LRESULT APIENTRY
	WndProc(HWND hWnd, const UINT msg, const WPARAM wParam, const LPARAM lParam) {
		if (imgui_initialized && imgui_intercept_input) {
			ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
			switch (msg) {
				case WM_LBUTTONDBLCLK:
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_RBUTTONDBLCLK:
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:
				case WM_MBUTTONDBLCLK:
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:
				case WM_MOUSEWHEEL:
				case WM_MOUSEMOVE:
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP:
				case WM_CHAR: return imgui_intercept_input ? 0 : CallWindowProc(game_wnd_proc, hWnd, msg, wParam, lParam);
				default: break;
			}
		}

		return CallWindowProc(game_wnd_proc, hWnd, msg, wParam, lParam);
	}

	using get_raw_input_data_t = UINT(WINAPI *)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
	get_raw_input_data_t game_get_raw_input_data = nullptr;

	UINT WINAPI
	get_raw_input_data(HRAWINPUT hRawInput, const UINT uiCommand, LPVOID pData, PUINT pcbSize, const UINT cbSizeHeader) {
		const auto result = game_get_raw_input_data(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
		if (auto *raw = static_cast<RAWINPUT *>(pData); pData && imgui_initialized && raw->header.dwType != RIM_TYPEHID) {
			if (result > 0) {
				if (raw->header.dwType == RIM_TYPEKEYBOARD &&
					raw->data.keyboard.Flags & RI_KEY_BREAK) {
					if (raw->data.keyboard.VKey == g_settings.overlay.toggle_key) {
						imgui_visible = !imgui_visible;
						imgui_intercept_input = imgui_visible;
						ToggleCursor();
					} else if (raw->data.keyboard.VKey == VK_ESCAPE && imgui_visible) {
						imgui_visible = false;
						imgui_intercept_input = false;
						ToggleCursor();
					} else if (raw->data.keyboard.VKey == g_settings.overlay.release_key && imgui_visible) {
						imgui_intercept_input = !imgui_intercept_input;
						ToggleCursor();
					} else {
						Overlay::HandleKeyPress(raw->data.keyboard.VKey);
					}

					return result;
				}
			}

			if (imgui_intercept_input) {
				const auto old = raw->header;
				memset(pData, 0, raw->header.dwSize);
				raw->header = old;
			}
		}

		return result;
	}

	auto
	destroy_swapchain(void* self) -> void* {
		reset();
		return game_destroy_swapchain(self);
	}

	auto
	Overlay::D3D12Init() -> void {
		g_output << "[overlay] loading dxgi and d3d12...\n";
		g_output.flush();

		create_window();

		LPVOID *target;

		if (MH_CreateHookApiEx(L"user32.dll",
							   "GetRawInputData",
							   reinterpret_cast<LPVOID>(&get_raw_input_data),
							   reinterpret_cast<LPVOID *>(&game_get_raw_input_data),
							   reinterpret_cast<LPVOID *>(&target)) == MH_OK) {
			MH_EnableHook(target);
		}

		auto **swapchain_vtable = static_cast<LPVOID*>(load_rel_var(find_address(SWAPCHAIN_VTABLE_SIGNATURE), SWAPCHAIN_VTABLE_ADDRESS));
		create_hook("SWAPCHAIN_VTABLE_DTOR",
			swapchain_vtable[SWAPCHAIN_VTABLE_DTOR],
			reinterpret_cast<LPVOID>(&destroy_swapchain),
			reinterpret_cast<LPVOID *>(&game_destroy_swapchain));

		using namespace Microsoft::WRL;

#define CLEANUP(msg)                   \
	g_output << "[overlay] " msg "\n"; \
	g_output.flush();                  \
	destroy_window();                  \
	return

		ComPtr<IDXGIFactory> dxgi_factory;
		if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&dxgi_factory)))) {
			CLEANUP("cannot get dxgi factory");
		}

		ComPtr<IDXGIAdapter> dxgi_adapter;
		if (dxgi_factory->EnumAdapters(0, dxgi_adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
			CLEANUP("cannot get dxgi adapter");
		}

		ComPtr<ID3D12Device> d3d12_device;
		if (FAILED(D3D12CreateDevice(dxgi_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12_device)))) {
			CLEANUP("cannot create d3d12 device");
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc;
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Priority = 0;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 0;

		ComPtr<ID3D12CommandQueue> d3d12_command_queue;
		if (FAILED(d3d12_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3d12_command_queue)))) {
			CLEANUP("cannot create d3d12 command queue");
		}

		DXGI_SWAP_CHAIN_DESC dxgi_swap_chain_desc = {};
		dxgi_swap_chain_desc.BufferDesc.Width = 64;
		dxgi_swap_chain_desc.BufferDesc.Height = 64;
		dxgi_swap_chain_desc.BufferDesc.RefreshRate.Numerator = 60;
		dxgi_swap_chain_desc.BufferDesc.RefreshRate.Denominator = 1;
		dxgi_swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		dxgi_swap_chain_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		dxgi_swap_chain_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		dxgi_swap_chain_desc.SampleDesc.Count = 1;
		dxgi_swap_chain_desc.SampleDesc.Quality = 0;
		dxgi_swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		dxgi_swap_chain_desc.BufferCount = 2;
		dxgi_swap_chain_desc.OutputWindow = window_handle;
		dxgi_swap_chain_desc.Windowed = 1;
		dxgi_swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		dxgi_swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

		ComPtr<IDXGISwapChain> dxgi_swap_chain;
		if (FAILED(dxgi_factory->CreateSwapChain(d3d12_command_queue.Get(), &dxgi_swap_chain_desc, &dxgi_swap_chain))) {
			CLEANUP("cannot create dxgi swap chain");
		}

		g_output << "[overlay] overwriting D3D12CommandQueue vtable...\n";
		g_output.flush();
		auto **d3d12_command_queue_vtable = *reinterpret_cast<LPVOID ***>(d3d12_command_queue.Get());
		create_hook("D3D12_COMMAND_QUEUE_VTABLE_EXECUTE_COMMAND_LISTS",
					d3d12_command_queue_vtable[D3D12_COMMAND_QUEUE_VTABLE_EXECUTE_COMMAND_LISTS],
					reinterpret_cast<LPVOID>(&execute_command_lists),
					reinterpret_cast<LPVOID *>(&game_execute_command_lists));

		g_output << "[overlay] overwriting DXGISwapChain vtable...\n";
		g_output.flush();
		auto **dxgi_swap_chain_vtable = *reinterpret_cast<LPVOID ***>(dxgi_swap_chain.Get());
		create_hook("DXGI_SWAP_CHAIN_VTABLE_PRESENT", dxgi_swap_chain_vtable[DXGI_SWAP_CHAIN_VTABLE_PRESENT], reinterpret_cast<LPVOID>(&present), reinterpret_cast<LPVOID *>(&game_present));
		create_hook("DXGI_SWAP_CHAIN_VTABLE_RESIZE_BUFFERS", dxgi_swap_chain_vtable[DXGI_SWAP_CHAIN_VTABLE_RESIZE_BUFFERS], reinterpret_cast<LPVOID>(&resize_buffers), reinterpret_cast<LPVOID *>(&game_resize_buffers));

		CLEANUP("finished");
	}
} // namespace rivet_hook
