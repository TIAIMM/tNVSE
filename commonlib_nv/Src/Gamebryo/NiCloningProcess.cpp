#include "NiCloningProcess.hpp"

// 0x4AD0C0
NiCloningProcess::NiCloningProcess(UInt32 uiHashSize) {
    m_pkCloneMap    = new NiTPointerMap<NiObject*, NiObject*>(uiHashSize);
    m_pkProcessMap  = new NiTPointerMap<NiObject*, bool>(uiHashSize);
    m_eCopyType     = NiObjectNET::GetDefaultCopyType();
    m_cAppendChar   = NiObjectNET::GetDefaultAppendCharacter();
}

// 0x4AD1D0
NiCloningProcess::~NiCloningProcess() {
    delete m_pkCloneMap;
    delete m_pkProcessMap;
}