#include "NiBillboardNode.hpp"
#include "NiCullingProcess.hpp"

void NiBillboardNode::SetMode(FaceMode eMode) {
	m_usFlags.SetField(eMode, FACE_MODE_MASK, FACE_MODE_POS);
}

NiBillboardNode::FaceMode NiBillboardNode::GetMode() const {
	return static_cast<FaceMode>(m_usFlags.GetField(FACE_MODE_MASK, FACE_MODE_POS));
}

void NiBillboardNode::RotateToCamera(NiCamera* apCamera) {
	ThisStdCall(0xA7DE40, this, apCamera);
}
