#include "TES.hpp"
#include "BGSTerrainManager.hpp"
#include "BSTextureManager.hpp"
#include "BSCullingProcess.hpp"
#include "ShadowSceneNode.hpp"
#include "TESWorldSpace.hpp"


TES* TES::GetSingleton() {
	return *(TES**)0x11DEA10;
}

Sky* TES::GetSky() {
	return GetSingleton()->pSky;
}

// GAME - 0x70EC90
TESWaterSystem* TES::GetWaterManger() {
    return GetSingleton()->pWaterManager;
}

// GAME - 0x4FD3E0
TESWorldSpace* TES::GetWorldSpace() {
	return GetSingleton()->pWorldSpace;
}

// GAME - 0x5F36F0
TESObjectCELL* TES::GetInterior() {
	return GetSingleton()->pInteriorCell;
}

// GAME - 0x457070
TESObjectCELL* TES::GetCurrentCell() {
    return ThisStdCall<TESObjectCELL*>(0x457070, this);
}

// GAME - 0x451900
TESObjectCELL* TES::GetGridCellCell(SInt32 aiX, SInt32 aiY) {
    return ThisStdCall<TESObjectCELL*>(0x451900, this, aiX, aiY);
}

// GAME - 0x45AF00
NavMeshInfoMap* TES::GetNavMeshInfoMap() {
	return ThisStdCall<NavMeshInfoMap*>(0x45AF00, this);
}

// GAME - 0x4539A0
void TES::ClearAllCells(bool abBufferedonly, bool abCurrentCell) {
	ThisStdCall(0x4539A0, this, abBufferedonly, abCurrentCell);
}

// GAME - 0x450D80
void TES::InitVars() {
    iCurrentGridX   = 0x7FFFFFFF;
    iCurrentGridY   = 0x7FFFFFFF;
    iCurrentQueuedX = 0x7FFFFFFF;
    iCurrentQueuedY = 0x7FFFFFFF;
    iSaveGridX      = 0x7FFFFFFF;
    iSaveGridY      = 0x7FFFFFFF;
}

// GAME - 0x4511E0
bool TES::IsCellLoaded(TESObjectCELL* apCell, bool abIgnoreBuffered) {
    return ThisStdCall(0x4511E0, this, apCell, abIgnoreBuffered);
}

// GAME - 0x458BE0
IO_TASK_PRIORITY TES::GetCellPriority(TESObjectCELL* apCell, NiPoint3* apPos) {
	return ThisStdCall<IO_TASK_PRIORITY>(0x458BE0, this, apCell, apPos);
}

// GAME - 0x451E40
bool TES::CanAttach3D(TESObjectREFR* apRef) {
	TESForm* pBase = apRef->pBaseForm;
    return !pBase->GetDeleted() && !pBase->GetDisabled();
}

// GAME - 0x457050
GridCell* TES::GetGridCell(SInt32 aX, SInt32 aY) {
    return ThisStdCall<GridCell*>(0x457050, this, aX, aY);
}

// GAME - 0x4572E0
bool TES::GetLandHeight(const NiPoint3& akPosition, float* afHeight) const {
    return ThisStdCall(0x4572E0, this, &akPosition, afHeight);
}

// GAME - 0x457620
NiGeometry* TES::GetLandGeometry(const NiPoint2& arPosition) const {
    return ThisStdCall<NiGeometry*>(0x457620, this, &arPosition);
}

// GAME - 0x4568C0
void TES::CreateTextureImage(const char* apPath, NiSourceTexturePtr& aspTexture, bool abNoFileOK, bool abArchiveOnly) {
    ThisStdCall(0x4568C0, this, apPath, &aspTexture, abNoFileOK, abArchiveOnly);
}

// GAME - 0x4C8180 - GECK
// void TES::SaveTextureImage(NiTexture* apTexture, const char* apPath, D3DXIMAGE_FILEFORMAT aeFormat) {
//     LPDIRECT3DBASETEXTURE9 pD3DTex = apTexture->GetDX9RendererData()->GetD3DTexture();
//     D3DXSaveTextureToFile(apPath, aeFormat, pD3DTex, nullptr);
// }

// GAME - 0x456800
bool TES::FreeTexture(const char* apPath, UInt32 auiMinRefCount) {
	return ThisStdCall(0x456800, this, apPath, auiMinRefCount);
}

// GAME - 0x45B070
void TES::UpdateMultiBoundVisibility(NiCamera* apCamera) {
    ThisStdCall(0x45B070, this, apCamera);
}

// GAME - 0x45BC80
void TES::ResetAllMultiBoundNodes(bool abLandscape, bool abRooms) {
    ThisStdCall(0x45BC80, this, abLandscape, abRooms);
}

// GAME - 0x458440, 0x458420
NiAVObject* TES::Pick(bhkPickData* apPickData, bool abHavok) const {
	return ThisStdCall<NiAVObject*>(0x458440, this, apPickData, abHavok);
}

// GAME - 0x455690
void TES::HandleCellLoopingSounds() {
	ThisStdCall(0x455690, this);
}

// GAME - 0x457BA0
NiObject* TES::CloneNiObject(NiObject* apObject, NiCloningProcess& arCloneProc) {
    if (!apObject)
        return nullptr;

    NiObject* pClone = apObject->CreateClone(&arCloneProc);
	CloneModelData(pClone);
	return pClone;
}

// GAME - 0x457A00
void TES::CloneModelData(NiObject* apObject) {
    CdeclCall(0x457A00, apObject);
}

// GAME - 0x455640
void TES::UpdateRefAnimations() {
	ThisStdCall(0x455640, this);
}
