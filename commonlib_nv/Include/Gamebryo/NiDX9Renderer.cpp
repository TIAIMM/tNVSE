#include "NiDX9Renderer.hpp"
#include "NiRenderTargetGroup.hpp"

NiDX9Renderer* NiDX9Renderer::Create(UInt32 auiWidth, UInt32 auiHeight, UInt32 auiUseFlags, HWND akWndDevice, HWND akWndFocus, UInt32 auiAdapter, DeviceDesc aeDesc, FrameBufferFormat aeFBFormat, DepthStencilFormat aeDSFormat, PresentationInterval aePresentationInterval, SwapEffect aeSwapEffect, UInt32 auiFBMode, UInt32 auiBackBufferCount, UInt32 auiRefreshRate, bool abUseD3D9ex) {
	return CdeclCall<NiDX9Renderer*>(0xE76210, auiWidth, auiHeight, auiUseFlags, akWndDevice, akWndFocus, auiAdapter, aeDesc, aeFBFormat, aeDSFormat, aePresentationInterval, aeSwapEffect, auiFBMode, auiBackBufferCount, auiRefreshRate, abUseD3D9ex);
}

NiDX9Renderer* NiDX9Renderer::GetSingleton() {
	return *reinterpret_cast<NiDX9Renderer**>(0x11C73B4);
}

LPDIRECT3D9 NiDX9Renderer::GetD3D9() {
	return *reinterpret_cast<LPDIRECT3D9*>(0x126F0D8);
}

LPDIRECT3DDEVICE9 NiDX9Renderer::GetD3DDevice() const {
	return m_pkD3DDevice9;
}

// 0xB6C1A0
UInt32 NiDX9Renderer::GetScreenWidth() {
	return ThisStdCall<UInt32>(0xB6C1A0, this);
}

// 0xB6C1D0
UInt32 NiDX9Renderer::GetScreenHeight() {
	return ThisStdCall<UInt32>(0xB6C1D0, this);
}