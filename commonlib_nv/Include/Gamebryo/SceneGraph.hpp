#pragma once

#include "BSSceneGraph.hpp"

NiSmartPointer(SceneGraph);

class SceneGraph : public BSSceneGraph {
public:
	double CalculateNoLODFarDistEx();

	static SceneGraph* Create(const char* apName, bool abMenuSceneGraph = false, NiCamera* apCamera = nullptr);

	__forceinline void SetCameraFOV(float afFOV, bool a3, NiCamera* apCamera, bool bSetLODAdjust) {
		ThisStdCall(0xC52020, this, afFOV, a3, apCamera, bSetLODAdjust);
	}

	static __forceinline void UpdateParticleShaderFoVData(float afFOV) {
		CdeclCall(0xB54000, afFOV);
	}

	NiCamera *GetCamera() {
		return ThisStdCall<NiCamera*>(0x6629F0, this);
	}

	NiNode* GetFlyCamNode() const {
		return static_cast<NiNode*>(GetAtSafely(0));
	}
};