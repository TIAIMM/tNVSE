#include "BSCullingProcess.hpp"
#include "BSCompoundFrustum.hpp"

BSCullingProcess::BSCullingProcess(NiVisibleArray* apVisibleSet) {
	eCullMode = BSCP_CULL_NORMAL;
	uiStackIndex = 0;
	pCompoundFrustum = nullptr;
	spAccumulator = nullptr;
}

// GAME - 0xC4EE90
void BSCullingProcess::Process(NiAVObject* apObject) {
	ThisStdCall(0xC4EE90, this, apObject);
}

// GAME - 0xC4F070
void BSCullingProcess::ProcessAlt(const NiCamera* apCamera, NiAVObject* apObject, NiVisibleArray* apVisibleSet) {
	ThisStdCall(0xC4F070, this, apCamera, apObject, apVisibleSet);
}

// GAME - 0xC4F1D0
void BSCullingProcess::AppendVirtual(NiGeometry* apGeom) {
	ThisStdCall(0xC4F1D0, this, apGeom);
}

void BSCullingProcess::SetCullMode(BSCPCullingType aeType) {
	eCullMode = aeType;
}

void BSCullingProcess::PushCullMode(BSCPCullingType aeType) {
	eTypeStack[uiStackIndex++] = eCullMode;
	eCullMode = aeType;
}

void BSCullingProcess::PopCullMode() {
	eCullMode = eTypeStack[--uiStackIndex];
}

void BSCullingProcess::SetAccumulator(BSShaderAccumulator* pkAccumulator) {
	spAccumulator = pkAccumulator;
}
bool BSCullingProcess::AddShared(NiAVObject* apObject) {
	return spAccumulator->AddShared(apObject);
}

void BSCullingProcess::ClearSharedMap() {
	spAccumulator->ClearSharedMap();
}