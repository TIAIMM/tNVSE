#pragma once

#include "NiAVObject.hpp"

NiSmartPointer(NiAVObjectPalette);

class NiAVObjectPalette : public NiObject {
public:
	NiAVObjectPalette();
	virtual ~NiAVObjectPalette();

	virtual NiAVObject*		GetAVObject(const char* pcName);
	virtual void			SetAVObject(const char* pcName, NiAVObject* apObject);
};

ASSERT_SIZE(NiAVObjectPalette, 0x8)