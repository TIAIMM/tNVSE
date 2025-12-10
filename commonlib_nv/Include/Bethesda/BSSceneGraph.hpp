#pragma once

#include "NiNode.hpp"

class BSCullingProcess;
class NiCamera;
class NiVisibleArray;

NiSmartPointer(BSSceneGraph);

class BSSceneGraph : public NiNode {
public:
	virtual double CalculateNoLODFarDist();
	virtual void   SetViewDistanceBasedOnFrameRate(float afTime);

	NiPointer<NiCamera> spCamera;
	NiVisibleArray*		pVisArray;
	BSCullingProcess*	pCuller;
	bool				bMenuSceneGraph;
	float				fCurrentFOV;

	NIRTTI_ADDRESS(0x1203180);

	__forceinline NiCamera* GetCamera() const { return spCamera; };
	__forceinline BSCullingProcess* GetCullingProcess() const { return pCuller; };
};

ASSERT_SIZE(BSSceneGraph, 0xC0);