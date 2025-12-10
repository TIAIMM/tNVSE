#pragma once

#include "NiMemObject.hpp"

class NiD3DRSEntry : public NiMemObject {
public:
	D3DRENDERSTATETYPE	m_uiRenderState;
	UInt32				m_uiValue;
	NiD3DRSEntry*		m_pkNext;
	NiD3DRSEntry*		m_pkPrev;
};

ASSERT_SIZE(NiD3DRSEntry, 0x10);