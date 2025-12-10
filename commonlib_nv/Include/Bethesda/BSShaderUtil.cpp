#include "BSShaderUtil.hpp"

// GAME - 0xB6BFC0
void BSShaderUtil::AccumulateGeometryList(const NiCamera* apCamera, void* apGeometryList, BSCullingProcess* apCullingProcess) {
	CdeclCall(0xB6BFC0, apCamera, apGeometryList, apCullingProcess);
}


// GAME - 0xB6C0D0
void BSShaderUtil::RenderScene(NiCamera* apCamera, BSShaderAccumulator* apAccumulator) {
	CdeclCall(0xB6C0D0, apCamera, apAccumulator);
}

// GAME - 0xB6BEE0
void __cdecl BSShaderUtil::AccumulateGeometry(const NiCamera* apCamera, NiAVObject* apNode, BSCullingProcess* apCullingProcess) {
	CdeclCall(0xB6BEE0, apCamera, apNode, apCullingProcess);
}