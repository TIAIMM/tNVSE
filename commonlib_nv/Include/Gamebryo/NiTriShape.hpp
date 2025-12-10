#pragma once

#include "NiTriBasedGeom.hpp"
#include "NiTriShapeData.hpp"

NiSmartPointer(NiTriShape);

class NiTriShape : public NiTriBasedGeom {
public:
	NiTriShape();
	virtual ~NiTriShape();

	CREATE_OBJECT(NiTriShape, 0xA744D0);

	static NiTriShape* Create(NiTriShapeData* apData);
	static NiTriShape* Create(UInt16 ausVertices, NiPoint3* apkVertex, NiPoint3* apkNormal, NiColorA* apkColor, NiPoint2* apkTexture, UInt16 ausNumTextureSets, UInt32 aeNBTMethod, UInt16 ausTriangles, UInt16* apusTriList);

	NiTriShapeData* GetModelData() const;

	UInt32 GetTriListLength();
	UInt16* GetTriList();

	void RenderImmediateEx(NiDX9Renderer* apRenderer);
	void RenderImmediateAltEx(NiDX9Renderer* apRenderer);
};

ASSERT_SIZE(NiTriShape, 0xC4);