#pragma once

#include "NiObjectNET.hpp"
#include "NiPoint3.hpp"
#include "NiTPointerMap.hpp"

class NiCloningProcess : public NiMemObject {
public:
	NiCloningProcess(UInt32 uiHashSize = 257);
	~NiCloningProcess();

	NiTPointerMap<NiObject*, NiObject*>*	m_pkCloneMap;
	NiTPointerMap<NiObject*, bool>*			m_pkProcessMap;
	NiObjectNET::CopyType					m_eCopyType;
	char									m_cAppendChar;
};

ASSERT_SIZE(NiCloningProcess, 0x10);