#pragma once

// D3D12/DXGI hooks of the host (lane host-dx12). Nothing happens until the
// game makes a D3D12 swapchain:
//
//   InstallD3D12()  (export RedemptionEditorHostInstallD3D12, called by
//                   whoever loads the host into a DX12 game; safe under the
//                   loader lock: it only starts a thread) watches for the
//                   system d3d12.dll and dxgi.dll the game loads itself (the
//                   host never loads them: a ReShade d3d12.dll/dxgi.dll in the
//                   game folder must keep its name) and hooks
//                   D3D12CreateDevice (MinHook);
//   D3D12CreateDevice  a device made (or, if none came through the hook 15 s
//                   after it went in, the watcher): hooks IDXGIFactory::CreateSwapChain and
//                   IDXGIFactory2::CreateSwapChainForHwnd (code of the system
//                   factory, found through a dummy factory's vtable);
//   CreateSwapChain*  on a D3D12 queue (the pDevice argument is the queue that
//                   presents): a D3D12Backend for the swapchain, and hooks of
//                   IDXGISwapChain::Present, IDXGISwapChain1::Present1,
//                   ResizeBuffers and IDXGISwapChain3::ResizeBuffers1;
//   Present         host::OnPresent between BeginPresent/EndPresent of the
//                   backend, then the real present.
//
// ID3D12CommandQueue::ExecuteCommandLists is NOT hooked: the presenting queue
// is the CreateSwapChain* argument (and IDXGISwapChain::GetDevice gives it
// back for a D3D12 swapchain), and a hook there would also catch the host's
// own submissions.
//
// ReShade: a module exporting ReShadeRegisterAddon in the process
// (the global ReShade Vulkan layer is loaded in DX12 mode too, for the game's
// enumeration instance, and hooks d3d12.dll/dxgi.dll itself) stops the D3D12
// path the same way: "HOST_D3D12 standing_down reason=reshade_in_process".
// Every detour logs its first 3 calls ("HOST_D3D12 enter fn=..."), and the
// host's work in every detour runs under SEH.
//
// Ownership (host/host_owner.h): while the add-on owns the editor in this
// process (its mutex exists before the host's first present) nothing is
// hooked past what is already there and no backend is made: "HOST_D3D12
// standing_down reason=editor_owned_by_reshade_addon".
// Any failure is logged ("HOST_D3D12 failed reason=...") and leaves the game
// alone; an exception inside the host's present work turns the host off for
// the rest of the session (the game's Present still runs).

namespace host::d3d12
{
    // Starts the watcher once. False only when the thread could not start.
    bool InstallD3D12();
}

extern "C" __declspec(dllexport) int __stdcall RedemptionEditorHostInstallD3D12();
