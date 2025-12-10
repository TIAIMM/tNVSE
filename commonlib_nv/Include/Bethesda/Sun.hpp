#pragma once

#include "NiTriShape.hpp"
#include "SkyObject.hpp"
#include "NiBillboardNode.hpp"
#include "BSShaderAccumulator.hpp"
#include "NiDirectionalLight.hpp"
#include "SkyShaderProperty.hpp"

class NiPick;

class Sun : public SkyObject {
public:
	NiBillboardNodePtr		spSunBaseNode;
	NiBillboardNodePtr		spSunGlareNode;
	NiTriShapePtr			spSunBase;
	NiTriShapePtr			spSunQuery;
	NiPick*					pSunPick; // Unused
	NiDirectionalLightPtr	spLight;
	float					fGlareScale;
	bool					bDoOcclusionTests;
	BSShaderAccumulatorPtr	spSunAccumulator;

	static bool bPhysical;

	void UpdateEx(Sky* apSky, float afDelta);

	void StartSunOcclusion();

	void CalculateSunGlare(UInt32 aiTestNumber, BSShaderAccumulator* apAccumulator, NiCamera* apCamera);

	float GetSunOcclusionPercent() const;

	void ClearDoOcclusionTests();

	float GetSunGlareAlpha() const;
};

ASSERT_SIZE(Sun, 0x2C);