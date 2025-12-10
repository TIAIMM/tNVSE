#pragma once

#include "NiTexturingProperty.hpp"
#include "NiD3DTextureStageGroup.hpp"
#include "NiDX9RenderState.hpp"

class NiGlobalConstantEntry;

NiSmartPointer(NiD3DTextureStage);

class NiD3DTextureStage {
public:
	NiD3DTextureStage();
	~NiD3DTextureStage();

	NiTexturingProperty::FilterMode		m_eFilter;
	UInt32								m_uiStage;
	NiTexture*							m_pkTexture;
	UInt32								m_uiTextureFlags;
	NiD3DTextureStageGroup*				m_pkTextureStageGroup;
	UInt32								m_uiTextureTransformFlags;
	NiGlobalConstantEntry*				m_pkGlobalEntry;
	D3DMATRIX							m_kTextureTransformation;
	UInt16								m_usObjTextureFlags;
	bool								m_bTextureTransform;
	bool								m_bRendererOwned;
	UInt32								m_uiRefCount;
	bool								bApply;
	bool								bUnk65;
	bool								bUnk66;
	bool								bSRGBTexture; // Added

	static float fMipMapLODBias;

	static NiDX9RenderState* GetRenderState();
	static NiDX9Renderer* GetRenderer();
	static LPDIRECT3DDEVICE9 GetD3DDevice();

	static void CreateNewStage(NiD3DTextureStagePtr& aspStage);

	void ApplyTexture();
	void ModifyClampMode(bool abNonPow2) const;
	void ApplyTextureTransform();
	bool ConfigureStage(bool abAllowRepeats = false);
	void SetTexture(NiTexture* apTexture);
	void SetClampMode(NiTexturingProperty::ClampMode eClamp);
	void SetFilterMode(NiTexturingProperty::FilterMode eFilter);
	void SetStageProperties(UInt32 auiTextureIndex, NiTexturingProperty::ClampMode aeClampMode, UInt32 aeFilterValue, bool abUseAnisotropicFilter = true);

	void SetFilterMode_Nearest(NiTexturingProperty::FilterMode eFilter);

	void ReturnStageToPool();

	void IncRefCount();
	void DecRefCount();

	NiD3DTextureStage* Constructor();
};

ASSERT_SIZE(NiD3DTextureStage, 0x68);