#pragma once

#include "BSShaderProperty.hpp"

enum BSCompareFunctions {
	BSCMP_ALWAYS		= 0,
	BSCMP_LESS			= 1,
	BSCMP_EQUAL			= 2,
	BSCMP_LESSEQUAL		= 3,
	BSCMP_GREATER		= 4,
	BSCMP_NOTEQUAL		= 5,
	BSCMP_GREATEREQUAL	= 6,
	BSCMP_NEVER			= 7,
};

enum BSBlendFunctions {
	BSBLEND_ONE				= 0,
	BSBLEND_ZERO			= 1,
	BSBLEND_SRCCOLOR		= 2,
	BSBLEND_INVSRCCOLOR		= 3,
	BSBLEND_DESTCOLOR		= 4,
	BSBLEND_INVDESTCOLOR	= 5,
	BSBLEND_SRCALPHA		= 6,
	BSBLEND_INVSRCALPHA		= 7,
	BSBLEND_DESTALPHA		= 8,
	BSBLEND_INVDESTALPHA	= 9,
	BSBLEND_SRCALPHASAT		= 10,
};


enum BSStencilBufferOperations {
	BSSTENCILOP_KEEP	= 0,
	BSSTENCILOP_ZERO	= 1,
	BSSTENCILOP_REPLACE = 2,
	BSSTENCILOP_INCR	= 3,
	BSSTENCILOP_DECR	= 4,
	BSSTENCILOP_INVERT	= 5,
};

enum RenderStateCounterType : UInt32 {
	kZBuffer		= 0,
	kZWrite			= 1,
	kZFunc			= 2,
	kDepthBias		= 3,
	kAlphaTest		= 4,
	kAlphaFunc		= 5,
	kAlphaBlend		= 6,
	kAlphaBlends	= 7,
	kBlendOp		= 8,
	kStencilEnable	= 9,
	kStencilActions = 10,
	kStencilFuncs	= 11,
	kStencilWrite	= 12,
	kScissorTest	= 13,
	// ??			= 14,
	kClipPlane		= 15,
	kColorWrite		= 16,
	kColorWrite123	= 17,
	kFillMode		= 18,
	kCullMode		= 19,
	kTMSAA			= 20,
	COUNT
};


class BSRenderState {
public:
	static RenderStateCounterType* const RenderStateCounters;
	static DWORD* const RenderStateStatus;

	static BSBlendFunctions SelectBlendType(D3DBLEND aeType);

	static void InitHooks();
	static void SetAlphaAntiAliasing(bool abEnable, bool abMarkStatus);
	static void ResetAlphaAntiAliasing(bool abMarkStatus);

	static void SetAlphaBlendEnable(bool abEnable, bool abMarkStatus);
	static void ResetAlphaBlendEnable(bool abMarkStatus);

	static void SetAlphaTestEnable(bool abEnable, bool abMarkStatus);
	static void ResetAlphaTestEnable(bool abMarkStatus);

	static void SetAlphaFuncAndRef(BSCompareFunctions aeCompareType, DWORD aiAlphaRef, bool abMarkStatus);
	static void ResetAlphaFuncAndRef(bool abMarkStatus);
	static void SetSrcAndDstBlends(BSBlendFunctions aeSrcType, BSBlendFunctions aeDestType, bool abMarkStatus);
	static void ResetSrcAndDstBlends(bool abMarkStatus);

	static void SetFillMode(bool abWireframe, bool abMarkStatus);
	static void ResetFillMode(bool abMarkStatus);

	
	static void SetColorWriteEnable(UInt32 aMask, bool abMarkStatus);
	static void ResetColorWriteEnable(bool abMarkStatus);


	static void SetScissorTestEnable(bool abEnable, bool abMarkStatus);
	static void SetScissorRectangle(SInt32 left, SInt32 top, SInt32 right, SInt32 bottom);
	static void AdjustScissorRectangle(SInt32 left, SInt32 top, SInt32 right, SInt32 bottom, bool abMarkStatus);
	static void ResetScissorRectangle();

	static void ResetZBuffer(bool abMarkStatus);

	static void ResetZFunc(bool abMarkStatus);
	static void SetZFunc(BSCompareFunctions aeFunc, bool abMarkStatus);

	static void SetZWriteEnable(bool abEnable, bool abMarkStatus);
	static void ResetZWriteEnable(bool abMarkStatus);

	static void SetZEnable(D3DZBUFFERTYPE aeBufferType, bool abMarkStatus);

	static void SetDepthBias(float afDepthBias, bool abMarkStatus);
	static void ResetDepthBias(bool abMarkStatus);

	static void SetStencilEnable(bool abEnable, bool abMarkStatus);
	static void SetStencilFunctions(BSCompareFunctions aeCompareType, int aiStencilRef, DWORD aiStencilMask, bool abMarkStatus);
	static void SetStencilOperations(BSStencilBufferOperations stencilFail, BSStencilBufferOperations stencilZFail, BSStencilBufferOperations stencilPass, bool abMarkStatus);
	static void SetStencilWriteMask(DWORD aMask, bool abMarkStatus);
	static void ResetStencil(bool abMarkStatus);

	static void SetCullMode(NiStencilProperty::DrawMode aeMode);

	static void SetClipPlaneEnable(bool abPlane0, bool abPlane1, bool abPlane2, bool abPlane3, bool abPlane4, bool abPlane5, bool abMarkStatus);
	static void ResetClipPlane(bool abMarkStatus);

	static void AddStatus(RenderStateCounterType type, bool abMark);
	static void RemoveStatus(RenderStateCounterType type, bool abMark);
	static void DecreaseStatus(RenderStateCounterType type);
	static void ResetStatus(RenderStateCounterType type);
	static UInt32 GetStatus(RenderStateCounterType type);

	static void SetRenderState(D3DRENDERSTATETYPE aeRenderStateType, DWORD value, bool abMarkStatus);

	static RenderStateCounterType GetStateCounter(D3DRENDERSTATETYPE aeRenderStateType);
};