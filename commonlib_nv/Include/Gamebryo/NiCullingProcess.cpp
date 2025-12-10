#include "NiCullingProcess.hpp"
#include "BSShaderAccumulator.hpp"

// GAME - 0xA69400
NiCullingProcess::NiCullingProcess(NiVisibleArray* apVisibleSet) {
	ThisStdCall(0xA69400, this, apVisibleSet);
}

// GAME - 0xA69640
void NiCullingProcess::Process(NiAVObject* apObject) {
	ThisStdCall(0xA69640, this, apObject);
}

// GAME - 0xA69570
void NiCullingProcess::ProcessAlt(const NiCamera* apCamera, NiAVObject* apScene, NiVisibleArray* apVisibleSet) {
	ThisStdCall(0xA69570, this, apCamera, apScene, apVisibleSet);
}

void NiCullingProcess::SetCamera(const NiCamera* apCamera) {
	m_pkCamera = const_cast<NiCamera*>(apCamera);
}

NiCamera* NiCullingProcess::GetCamera() const {
	return m_pkCamera;
}

// GAME - 0xA694A0
void NiCullingProcess::SetFrustum(const NiFrustum* kFrustum) {
	ThisStdCall(0xA694A0, this, kFrustum);
};

// GAME - 0xA694E0
void NiCullingProcess::DoCulling(NiAVObject* pkObject) {
	ThisStdCall(0xA694E0, this, pkObject);
}