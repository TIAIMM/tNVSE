#pragma once

#include "hkpAllCdPointCollector.hpp"

NiSmartPointer(hkpWorldObject);

__declspec(align(16)) class bhkCharacterPointCollector : public hkpAllCdPointCollector {
public:
	bhkCharacterPointCollector* pStartCollector;
	hkArray<hkpWorldObjectPtr>	contactBodies;
	hkArray<UInt32>				arr3B0;
	hkArray<hkpRootCdPoint>		arr3BC;
};

ASSERT_SIZE(bhkCharacterPointCollector, 0x3D0);