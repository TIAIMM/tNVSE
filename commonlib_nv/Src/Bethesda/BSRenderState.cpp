#include "BSRenderState.hpp"
#include "BSBatchRenderer.hpp"
#include "TESRenderer.hpp"
#include "NiD3DShader.hpp"

RenderStateCounterType* const BSRenderState::RenderStateCounters = (RenderStateCounterType*)0x11FFA30;
DWORD* const BSRenderState::RenderStateStatus = reinterpret_cast<DWORD*>(0x11FF9D8);

static constexpr D3DCMPFUNC	CompareFunctions[8] = {
	D3DCMP_ALWAYS,
	D3DCMP_LESS,
	D3DCMP_EQUAL,
	D3DCMP_LESSEQUAL,
	D3DCMP_GREATER,
	D3DCMP_NOTEQUAL,
	D3DCMP_GREATEREQUAL,
	D3DCMP_NEVER,
};

static constexpr D3DBLEND BlendFunctions[12] = {
	D3DBLEND_ONE,
	D3DBLEND_ZERO,
	D3DBLEND_SRCCOLOR,
	D3DBLEND_INVSRCCOLOR,
	D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
	D3DBLEND_SRCALPHASAT,
};

static constexpr D3DSTENCILOP StencilOperations[6] = {
	D3DSTENCILOP_KEEP,
	D3DSTENCILOP_ZERO,
	D3DSTENCILOP_REPLACE,
	D3DSTENCILOP_INCR,
	D3DSTENCILOP_DECR,
	D3DSTENCILOP_INVERT,
};

static constexpr BSBlendFunctions D3DBlendToBSBlend[12] = {
	BSBLEND_ZERO,			// 0 D3DBLEND_ZERO
	BSBLEND_ZERO,			// 1 D3DBLEND_ZERO
	BSBLEND_ONE,			// 2 D3DBLEND_ONE
	BSBLEND_SRCCOLOR,		// 3 D3DBLEND_SRCCOLOR
	BSBLEND_INVSRCCOLOR,	// 4 D3DBLEND_INVSRCCOLOR
	BSBLEND_SRCALPHA,		// 5 D3DBLEND_SRCALPHA
	BSBLEND_INVSRCALPHA,	// 6 D3DBLEND_INVSRCALPHA
	BSBLEND_DESTALPHA,		// 7 D3DBLEND_DESTALPHA
	BSBLEND_INVDESTALPHA,	// 8 D3DBLEND_INVDESTALPHA
	BSBLEND_DESTCOLOR,		// 9 D3DBLEND_DESTCOLOR
	BSBLEND_INVDESTCOLOR,	// 10 D3DBLEND_INVDESTCOLOR
	BSBLEND_SRCALPHASAT,	// 11 D3DBLEND_SRCALPHASAT
};

BSBlendFunctions BSRenderState::SelectBlendType(D3DBLEND aeType) {
	return D3DBlendToBSBlend[aeType];
}

// GAME - 0xB98540
// GECK - 0x931850
void BSRenderState::SetAlphaAntiAliasing(bool abEnable, bool abMarkStatus) {
	CdeclCall(0xB98540, abEnable, abMarkStatus);
}

void BSRenderState::ResetAlphaAntiAliasing(bool abMarkStatus) {
	RemoveStatus(kTMSAA, abMarkStatus);
	SetAlphaAntiAliasing(false, false);
}

// GAME - 0xB97FA0
// GECK - 0x9312B0
void BSRenderState::SetAlphaBlendEnable(bool abEnable, bool abMarkStatus) {
	SetRenderState(D3DRS_ALPHABLENDENABLE, abEnable, abMarkStatus);
}

// GAME - 0x714AE0
// GECK - 0x902DF0
void BSRenderState::ResetAlphaBlendEnable(bool abMarkStatus) {
	RemoveStatus(kAlphaBlend, abMarkStatus);
	SetAlphaBlendEnable(false, false);
}

// GAME - 0xB97ED0
// GECK - 0x9311E0
void BSRenderState::SetAlphaTestEnable(bool abEnable, bool abMarkStatus) {
	SetRenderState(D3DRS_ALPHATESTENABLE, abEnable, abMarkStatus);
}

// GAME - 0x714AA0
// GECK - 0x931980
void BSRenderState::ResetAlphaTestEnable(bool abMarkStatus) {
	RemoveStatus(kAlphaTest, abMarkStatus);
	SetAlphaTestEnable(false, false);
}

// GAME - 0xB98480
// GECK - 0x931790
void BSRenderState::SetFillMode(bool abWireframe, bool abMarkStatus) {
	SetRenderState(D3DRS_FILLMODE, !abWireframe + 2, abMarkStatus);
}

// GAME - 0x714C20
// GECK - None
void BSRenderState::ResetFillMode(bool abMarkStatus) {
	RemoveStatus(kFillMode, abMarkStatus);
	SetFillMode(false, false);
}

// GAME - 0xB97D20
// GECK - 0x931030
void BSRenderState::AdjustScissorRectangle(SInt32 left, SInt32 top, SInt32 right, SInt32 bottom, bool abMarkStatus) {
	CdeclCall(0xB97D20, left, top, right, bottom, abMarkStatus);
}

// GAME - 0xB98380
// GECK - 0x931690
void BSRenderState::SetColorWriteEnable(UInt32 aMask, bool abMarkStatus) {
	SetRenderState(D3DRS_COLORWRITEENABLE, aMask, abMarkStatus);
}

// GAME - 0x4ECED0
// GECK - 0x531C20
void BSRenderState::ResetColorWriteEnable(bool abMarkStatus) {
	RemoveStatus(kColorWrite, abMarkStatus);
	SetColorWriteEnable(D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, false);
}

// GAME - 0xB984F0
// GECK - 0x931800
void BSRenderState::SetScissorTestEnable(bool abEnable, bool abMarkStatus) {
	SetRenderState(D3DRS_SCISSORTESTENABLE, abEnable, abMarkStatus);
}

// GAME - 0xB97F20
// GECK - 0x931230
void BSRenderState::SetAlphaFuncAndRef(BSCompareFunctions aeCompareType, DWORD aiAlphaRef, bool abMarkStatus) {
	SetRenderState(D3DRS_ALPHAFUNC, CompareFunctions[aeCompareType], abMarkStatus);
	SetRenderState(D3DRS_ALPHAREF, aiAlphaRef, abMarkStatus);
}

// GAME - 0x714AC0
// GECK - 0x9319C0
void BSRenderState::ResetAlphaFuncAndRef(bool abMarkStatus) {
	RemoveStatus(kAlphaFunc, abMarkStatus);
	SetAlphaFuncAndRef(BSCMP_ALWAYS, 0, false);
}

// GAME - 0xB97FF0
// GECK - 0x931300
void BSRenderState::SetSrcAndDstBlends(BSBlendFunctions aeSrcType, BSBlendFunctions aeDestType, bool abMarkStatus) {
	SetRenderState(D3DRS_SRCBLEND, BlendFunctions[aeSrcType], abMarkStatus);
	SetRenderState(D3DRS_DESTBLEND, BlendFunctions[aeDestType], abMarkStatus);
}

// GAME - 0x714B00
// GECK - 0x9319E0
void BSRenderState::ResetSrcAndDstBlends(bool abMarkStatus) {
	RemoveStatus(kAlphaBlends, abMarkStatus);
	SetSrcAndDstBlends(BSBLEND_ONE, BSBLEND_ZERO, false);
}

// GAME - 0xB97E30
// GECK - 0x931140
void BSRenderState::SetZWriteEnable(bool abEnable, bool abMarkStatus) {
	SetRenderState(D3DRS_ZWRITEENABLE, abEnable, abMarkStatus);
}

// GAME - 0x714A60
// GECK - 0x902DD0
void BSRenderState::ResetZWriteEnable(bool abMarkStatus) {
	RemoveStatus(kZWrite, abMarkStatus);
	SetZWriteEnable(true, false);
}

// GAME - 0xB98070
// GECK - 0x931380
void BSRenderState::SetStencilEnable(bool abEnable, bool abMarkStatus) {
	SetRenderState(D3DRS_STENCILENABLE, abEnable, abMarkStatus);
}

// GAME - 0xB98180
// GECK - 0x931490
void BSRenderState::SetStencilFunctions(BSCompareFunctions aeCompareType, int aiStencilRef, DWORD aiStencilMask, bool abMarkStatus) {
	SetRenderState(D3DRS_STENCILFUNC, CompareFunctions[aeCompareType], false);
	SetRenderState(D3DRS_STENCILREF, aiStencilRef, false);
	SetRenderState(D3DRS_STENCILMASK, aiStencilMask, abMarkStatus);
}

// GAME - 0xB980C0
// GECK - 0x9313D0
void BSRenderState::SetStencilOperations(BSStencilBufferOperations stencilFail, BSStencilBufferOperations stencilZFail, BSStencilBufferOperations stencilPass, bool abMarkStatus) {
	SetRenderState(D3DRS_STENCILFAIL, StencilOperations[stencilFail], false);
	SetRenderState(D3DRS_STENCILZFAIL, StencilOperations[stencilZFail], false);
	SetRenderState(D3DRS_STENCILPASS, StencilOperations[stencilPass], abMarkStatus);
}

// GAME - 0xB98230
// GECK - 0x931540
void BSRenderState::SetStencilWriteMask(DWORD aMask, bool abMarkStatus) {
	SetRenderState(D3DRS_STENCILWRITEMASK, aMask, abMarkStatus);
}

// GAME - 0x4ECB40
// GECK - 0x531C40
void BSRenderState::ResetStencil(bool abMarkStatus) {
	RemoveStatus(kStencilEnable, abMarkStatus);
	SetStencilEnable(false, false);
}

// GAME - 0xB984D0
// GECK - 0x9317E0
void BSRenderState::SetCullMode(NiStencilProperty::DrawMode aeMode) {
	CdeclCall(0xB984D0, aeMode);
}

// GAME - 0xB97DE0
// GECK - 0x9310F0
void BSRenderState::SetZEnable(D3DZBUFFERTYPE aeBufferType, bool abMarkStatus) {
	SetRenderState(D3DRS_ZENABLE, aeBufferType, abMarkStatus);
}

// GAME - 0xB98320
// GECK - 0x931630
void BSRenderState::SetDepthBias(float afDepthBias, bool abMarkStatus) {
	SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&afDepthBias, abMarkStatus);
}

// GAME - 0x714BF0
// GECK - 0x902E10
void BSRenderState::ResetDepthBias(bool abMarkStatus) {
	RemoveStatus(kDepthBias, abMarkStatus);
	SetDepthBias(0.0f, false);
}

// GAME - 0xB98280
// GECK - 0x931590
void BSRenderState::SetClipPlaneEnable(bool abPlane0, bool abPlane1, bool abPlane2, bool abPlane3, bool abPlane4, bool abPlane5, bool abMarkStatus) {
	SetRenderState(D3DRS_CLIPPLANEENABLE, 
		(abPlane5 ? D3DCLIPPLANE5 : 0) | (abPlane4 ? D3DCLIPPLANE4 : 0) |
		(abPlane3 ? D3DCLIPPLANE3 : 0) | (abPlane2 ? D3DCLIPPLANE2 : 0) |
		(abPlane1 ? D3DCLIPPLANE1 : 0) | (abPlane0 ? D3DCLIPPLANE0 : 0), abMarkStatus);
}

// GAME - 0x4EB510
// GECK - 0x531C60
void BSRenderState::ResetClipPlane(bool abMarkStatus) {
	RemoveStatus(kClipPlane, abMarkStatus);
	SetClipPlaneEnable(false, false, false, false, false, false, false);
}

// GAME - 0xB4F540
// GECK - None
void BSRenderState::SetScissorRectangle(SInt32 left, SInt32 top, SInt32 right, SInt32 bottom) {
	AdjustScissorRectangle(left, top, right, bottom, true);
	SetScissorTestEnable(true, true);
}

// GAME - 0xB540D0
// GECK - None
void BSRenderState::ResetScissorRectangle() {
	DecreaseStatus(kClipPlane);
	DecreaseStatus(kScissorTest);
	SetScissorTestEnable(false, false);
}

// GAME - 0x714A40
// GECK - 0x931900
void BSRenderState::ResetZBuffer(bool abMarkStatus) {
	RemoveStatus(kZBuffer, abMarkStatus);
	SetZEnable(D3DZB_TRUE, false);
}

// GAME - 0x714A80
void BSRenderState::ResetZFunc(bool abMarkStatus) {
	RemoveStatus(kZFunc, abMarkStatus);
	SetZFunc(BSCMP_LESSEQUAL, false);
}

// GAME - 0xB97E80
// GECK - 0x931190
void BSRenderState::SetZFunc(BSCompareFunctions aeFunc, bool abMarkStatus) {
	SetRenderState(D3DRS_ZFUNC, CompareFunctions[aeFunc], abMarkStatus);
}

void BSRenderState::AddStatus(RenderStateCounterType type, bool abMark) {
	DWORD* setting = &RenderStateStatus[type];
	*setting += abMark;
}

// Based on 0x714AEB
void BSRenderState::RemoveStatus(RenderStateCounterType type, bool abMark) {
	DWORD* setting = &RenderStateStatus[type];
	*setting -= abMark;
}

void BSRenderState::DecreaseStatus(RenderStateCounterType type) {
	--RenderStateStatus[type];
}

// GAME - 0x4ECB10
// GECK - None
void BSRenderState::ResetStatus(RenderStateCounterType type) {
	if (GetStatus(type))
		DecreaseStatus(type);
}

UInt32 BSRenderState::GetStatus(RenderStateCounterType type) {
	return RenderStateStatus[type];
}

// GAME - 0xB97D90
// GECK - 0x9310A0
void BSRenderState::SetRenderState(D3DRENDERSTATETYPE aeRenderStateType, DWORD value, bool abMarkStatus) {
	CdeclCall(0xB97D90, aeRenderStateType, value, abMarkStatus);
}

RenderStateCounterType BSRenderState::GetStateCounter(D3DRENDERSTATETYPE aeRenderStateType) {
	return RenderStateCounters[aeRenderStateType];
}
