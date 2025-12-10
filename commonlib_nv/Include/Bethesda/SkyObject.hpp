#pragma once

#include "NiSmartPointer.hpp"

class Sky;
NiSmartPointer(NiNode);

class SkyObject {
public:
	virtual				~SkyObject();
	virtual NiNode*		GetRoot() const;
	virtual void		Initialize(NiNode* apRoot);
	virtual void		Update(Sky* apSky, float afValue);

	NiNodePtr spRoot;
};

ASSERT_SIZE(SkyObject, 0x8)