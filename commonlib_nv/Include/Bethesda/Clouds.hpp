#pragma once

#include "SkyObject.hpp"
#include "NiColor.hpp"

NiSmartPointer(NiGeometry);
NiSmartPointer(NiSourceTexture);

class Clouds : public SkyObject {
public:
	NiGeometryPtr		spClouds[4];
	NiSourceTexturePtr	spTransTexture[4];
	NiColor				pColors[4];
	UInt16				usNumLayers;
	bool				bForceUpdate;
};

ASSERT_SIZE(Clouds, 0x5C);