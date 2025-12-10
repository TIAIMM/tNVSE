#pragma once

#include "NiRenderedTexture.hpp"
#include "NiRenderTargetGroup.hpp"
#include "NiRenderedCubeMap.hpp"
#include "BSString.hpp"
#include "NiDX9TextureData.hpp"

NiSmartPointer(BSRenderedTexture);

class BSRenderedTexture : public NiObject {
public:
	NiRenderTargetGroupPtr spRenderTargetGroups[6];
	NiRenderTargetGroupPtr spSomeRT;
	NiObjectPtr spObject024;
	NiObjectPtr spObject028;
	SInt32 eType; // BSTextureManager::RenderTargetTypes
	NiRenderedTexturePtr spRenderedTextures[4];

	static inline const NiRTTI ms_RTTI = NiRTTI("BSRenderedTexture", NiObject::ms_RTTI);

	inline const NiRTTI* GetRTTIEx() {
		return &BSRenderedTexture::ms_RTTI;
	}

	static bool ReleaseUnusedRenderTargets();
	static void ReleaseCurrentRenderTarget();

	static BSRenderedTexture* Create(NiFixedString* apName, uint32_t width, uint32_t height, NiTexture::FormatPrefs* prefs, Ni2DBuffer::MultiSamplePreference eMSAAPref,
	                                 bool bUseDepthStencil, NiDepthStencilBuffer* pkDSBuffer, int a7, uint32_t uiBackgroundColor) {
		return CdeclCall<BSRenderedTexture*>(0xB6B610, apName, width, height, prefs, eMSAAPref, bUseDepthStencil, pkDSBuffer, a7, uiBackgroundColor);
	}

	static void SwitchRenderTarget() {
		CdeclCall(0xB6B840);
	}
};

ASSERT_SIZE(BSRenderedTexture, 0x40);
