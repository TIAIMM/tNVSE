#pragma once

#include "d3d9caps.h"
#include "NiD3DRenderState.hpp"

class NiDX9RenderState : public NiD3DRenderState {
public:
	NiDX9RenderState();
	virtual ~NiDX9RenderState();

	bool							m_bDeclaration;
	UInt32							m_uiCurrentFVF;
	UInt32							m_uiPreviousFVF;
	LPDIRECT3DVERTEXDECLARATION9	m_hCurrentVertexDeclaration;
	LPDIRECT3DVERTEXDECLARATION9	m_hPreviousVertexDeclaration;
	bool							m_bUsingSoftwareVP;
	D3DCAPS9						m_kD3DCaps9;

	static UInt16 GetSampleStatesRevMappings(UInt32 auiState);

	void SetDeclaration(LPDIRECT3DVERTEXDECLARATION9 apDeclaration, bool abSave = false);
	void SetSamplerStateEx(UInt32 auiStage, D3DSAMPLERSTATETYPE aeState, UInt32 auiValue, bool abSave = false);
	void SetCullMode(NiStencilProperty::DrawMode aeMode, bool abSave = false);
};

ASSERT_SIZE(NiDX9RenderState, 0x1248);