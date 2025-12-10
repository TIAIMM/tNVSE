#pragma once

#include "NiProperty.hpp"

NiSmartPointer(NiCullingProperty);

class  NiCullingProperty : public NiProperty {
public:
	UInt16 m_usFlags;
};

ASSERT_SIZE(NiCullingProperty, 0x1C)