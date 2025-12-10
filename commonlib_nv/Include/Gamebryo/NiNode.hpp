#pragma once

#include <vector>

#include "NiAVObject.hpp"
#include "NiTArray.hpp"

NiSmartPointer(NiNode);

class NiFixedString;

class NiNode : public NiAVObject {
public:
	NiNode();
	virtual ~NiNode();

	virtual void	AttachChild(NiAVObject* apChild, bool abFirstAvail);
	virtual void	InsertChildAt(UInt32 i, NiAVObject* apChild);
	virtual void	DetachChild(NiAVObject* apChild, NiAVObjectPtr& arResult);
	virtual void	DetachChildAlt(NiAVObject* apChild);
	virtual void	DetachChildAt(UInt32 i, NiAVObjectPtr& arResult);
	virtual void	DetachChildAtAlt(UInt32 i);
	virtual void	SetAt(UInt32 i, NiAVObject* apChild, NiAVObjectPtr& arResult);
	virtual void	SetAtAlt(UInt32 i, NiAVObject* apChild);
	virtual void	UpdateUpwardPass();

	NiTObjectArray<NiAVObjectPtr> m_kChildren;

	CREATE_OBJECT(NiNode, 0xA5F030);
	NIRTTI_ADDRESS(0x11F4428);

	static NiNode* Create(UInt16 ausChildCount = 0);

	UInt32 GetArrayCount() const;
	UInt32 GetChildCount() const;
	NiAVObject* GetAt(UInt32 i) const;
	NiAVObject* GetAtSafely(UInt32 i) const;
	NiAVObject* GetLastChild();
	void CompactChildArray();

	NiNode* FindObjectByName(const NiFixedString& akName);
	std::vector<NiNode*> FindObjectsByName(const NiFixedString& akName);

	void UpdatePropertiesUpward(NiPropertyState*& apParentState);

	void UpdateDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags);
	void UpdateSelectedDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags);
	void UpdateRigidDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags);
	void ApplyTransformEx(NiMatrix3& kMat, NiPoint3& kTrn, bool abOnLeft);

	static void SetFlagRecurse(NiNode* apNode, UInt32 auiFlag, bool abSet);

	template <typename FUNC>
	void ForEachChild(FUNC&& afn) {
		for (UInt32 i = 0; i < GetArrayCount(); i++) {
			NiAVObject* pChild = GetAt(i);
			if (pChild)
				afn(pChild);
		}
	}

	template <typename FUNC>
	void ForEachChildRecurse(FUNC&& afn) {
		ForEachChild([&](NiAVObject* apChild) {
			if (afn(apChild) && apChild->IsNiNode()) {
				static_cast<NiNode*>(apChild)->ForEachChildRecurse(afn);
			}
		});
	}
};

ASSERT_SIZE(NiNode, 0xAC);