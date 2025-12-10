#pragma once

#include "NiBackToFrontAccumulator.hpp"

NiSmartPointer(NiAlphaAccumulator);

class NiAlphaAccumulator : public NiBackToFrontAccumulator {
public:
	NiAlphaAccumulator();
	virtual ~NiAlphaAccumulator();

	bool m_bObserveNoSortHint;
	bool m_bSortByClosestPoint;
	bool m_bInterfaceSort;

	CREATE_OBJECT(NiAlphaAccumulator, 0xA9B4B0);

	void SortEx();
};

ASSERT_SIZE(NiAlphaAccumulator, 0x34);