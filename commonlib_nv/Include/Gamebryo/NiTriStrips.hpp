#pragma once

#include "NiTriBasedGeom.hpp"
#include "NiTriStripsData.hpp"

NiSmartPointer(NiTriStrips);

class NiTriStrips : public NiTriBasedGeom {
public:
	NiTriStrips();
	virtual ~NiTriStrips();

	CREATE_OBJECT(NiTriStrips, 0xA71CE0);

	static NiTriStrips* Create(UInt16 ausVertices, NiPoint3* apVertex, NiPoint3* apNormal, NiColorA* apColor, NiPoint2* apTexture, UInt16 ausNumTextureSets, NiGeometryData::DataFlags aeNBTMethod, UInt16 ausTriangles, UInt16 ausStrips, UInt16* apusStripLengths, UInt16* apusStripLists);

	NiTriStripsData* GetModelData() const;

	void RenderImmediateEx(NiDX9Renderer* apRenderer);
	void RenderImmediateAltEx(NiDX9Renderer* apRenderer);

	NIRTTI_ADDRESS(0x12682E8)
};

ASSERT_SIZE(NiTriStrips, 0xC4)