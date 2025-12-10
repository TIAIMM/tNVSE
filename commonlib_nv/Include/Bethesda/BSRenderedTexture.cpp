#include "BSRenderedTexture.hpp"
#include "NiDX9Renderer.hpp"

// 0xB6B6C0
bool BSRenderedTexture::ReleaseUnusedRenderTargets() {
	return CdeclCall<bool>(0xB6B6C0);
}

// 0xB6B0B0
void BSRenderedTexture::ReleaseCurrentRenderTarget() {
	CdeclCall(0xB6B0B0);
}