#pragma once

#include "NiMemObject.hpp"
#include "NiD3DRSEntry.hpp"

class NiD3DRenderStateGroup : public NiMemObject {
public:
	bool			m_bRendererOwned;
	UInt32			m_uiStateCount;
	NiD3DRSEntry*	m_pkRenderStates;
	UInt32			m_uiSavedStateCount;
	NiD3DRSEntry*	m_pkSavedRenderStates;

	static NiD3DRenderStateGroup* GetFreeRenderStateGroup();
	void SetRenderState(D3DRENDERSTATETYPE auiRenderState, UInt32 auiValue, bool abSave);

	NiD3DRSEntry* FindRenderStateEntry(UInt32 auiState, bool& abInSaveList);
};

ASSERT_SIZE(NiD3DRenderStateGroup, 0x14);