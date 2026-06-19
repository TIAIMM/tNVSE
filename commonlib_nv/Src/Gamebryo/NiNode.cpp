#include "NiNode.hpp"

#include <vector>

#include "NiCullingProcess.hpp"

NiNode* NiNode::Create(UInt16 ausChildCount) {
    return NiCreate<NiNode, 0xA5ECB0>(ausChildCount);
}

UInt32 NiNode::GetArrayCount() const {
	return m_kChildren.GetSize();
}

UInt32 NiNode::GetChildCount() const {
	return m_kChildren.GetEffectiveSize();
}

NiAVObject* NiNode::GetAt(UInt32 i) const {
	return m_kChildren.GetAt(i).m_pObject;
}

NiAVObject* NiNode::GetAtSafely(UInt32 i) const {
    if (GetArrayCount() <= i)
        return nullptr;
    
    return GetAt(i);
}

NiAVObject* NiNode::GetLastChild() {
    if (GetChildCount() == 0)
		return nullptr;

    return GetAt(GetChildCount() - 1);
}

// 0x4ADD70
void NiNode::CompactChildArray() {
    m_kChildren.Compact();
    m_kChildren.UpdateSize();
}

NiNode* NiNode::FindObjectByName(const NiFixedString& akName) {
    if (m_kName.m_kHandle && m_kName == akName) {
	    return this;
    }

    if (!GetArrayCount()) {
	    return nullptr;
    }

    for (UInt32 i = 0; i < GetArrayCount(); i++) {
        NiAVObject* pkChild = GetAt(i);
        if (pkChild && IS_NODE(pkChild)) {
	        if (NiNode* pkObject = pkChild->NiDynamicCast<NiNode>()->FindObjectByName(akName)) {
		        return pkObject;
	        }
        }
    }

    return nullptr;
}

std::vector<NiNode*> NiNode::FindObjectsByName(const NiFixedString& akName) {
    std::vector<NiNode*> res{};
    if (m_kName.m_kHandle && m_kName == akName) {
        res.push_back(this);
    }

    if (!GetArrayCount()) {
        return res;
    }

    for (UInt32 i = 0; i < GetArrayCount(); i++) {
        NiAVObject* pkChild = GetAt(i);
        if (pkChild && IS_NODE(pkChild)) {
            auto curRes = pkChild->NiDynamicCast<NiNode>()->FindObjectsByName(akName);
            res.insert(res.end(), curRes.begin(), curRes.end());
        }
    }

    return res;
}

// 0xA5E3A0
void NiNode::UpdatePropertiesUpward(NiPropertyState*& apParentState) {
    ThisStdCall(0xA5E3A0, this, &apParentState);
}

// 0xA5DD70
void NiNode::UpdateDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags) {
    ThisStdCall(0xA5DD70, this, &arData, auiFlags);
}

// 0xA5DED0
void NiNode::UpdateSelectedDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags) {
    ThisStdCall(0xA5DED0, this, &arData, auiFlags);
}

// 0xA5E1F0
void NiNode::UpdateRigidDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags) {
    ThisStdCall(0xA5E1F0, &arData, auiFlags);
}

// 0xA5D940
void NiNode::ApplyTransformEx(NiMatrix3& kMat, NiPoint3& kTrn, bool abOnLeft) {
    ThisStdCall(0xA5D940, this, &kMat, &kTrn, abOnLeft);
}

void NiNode::SetFlagRecurse(NiNode* apNode, UInt32 auiFlag, bool abSet) {
    apNode->SetBit(auiFlag, abSet);

    for (UInt32 i = 0; i < apNode->GetArrayCount(); i++) {
		NiAVObject* pChild = apNode->GetAt(i);
        if (pChild) {
            pChild->SetBit(auiFlag, abSet);

            if (IS_NODE(pChild)) {
	            SetFlagRecurse(static_cast<NiNode*>(pChild), auiFlag, abSet);
            }
        }
	}
}
