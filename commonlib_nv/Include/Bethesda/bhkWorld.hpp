#pragma once

#include "bhkSerializable.hpp"
#include "BSSimpleList.hpp"
#include "hkArray.hpp"
#include "hkVector4.hpp"
#include "NiAVObject.hpp"

class bhkNiCollisionObject;
class TESTrapListener;
class TESWaterListener;
class BGSAcousticSpaceListener;
class hkpConstraintInstance;
class bhkEntityListener;
class bhkPickData;
class hkpAction;

__declspec(align(16)) class bhkWorld : public bhkSerializable {
public:
	virtual void Update();
	virtual void PickObject(bhkPickData& arPickData);
	virtual bool HasActiveSimIslands();
	virtual void AddObjects(NiAVObject* apObject, bool abRecurse, bool abNotify, int aiGroup, bool abForce);
	virtual void Unk_53(NiAVObject*, NiAVObject*);
	virtual void SetDebugDisplay(void*, bool abTF);

	struct ObjectRecData {
		bhkWorld*	pWorld;
		bool		bRecurse;
		UInt32		eAction;
		UInt32		uData[4];
	};

	typedef void(__cdecl *pfnRecFunc)(bhkNiCollisionObject* apHkColObject, ObjectRecData& apData);

	NiRefObject*					pVisDebug;
	bool							bAdded;
	bool							bEnabled;
	bool							bDebugDisplay;
	UInt32							uiBatchAdd;
	UInt32							uiBatchRemove;
	TESTrapListener*				pTrapListener;
	TESWaterListener*				pWaterListener;
	BGSAcousticSpaceListener*		pAcousticSpaceListener;
	void**							pEntityBatch;
	void*							iEntityBatchCount;
	void**							unk_34;
	void*							unk_38;
	hkpAction**						pActionBatch;
	void*							iActionBatchCount;
	hkpConstraintInstance**			pConstraintInstanceBatch;
	SInt32							uiConstraintInstanceBatchCount;
	void**							pEntityRemoveBatch;
	UInt32							uiEntityRemoveBatchCount;
	UInt32							unk54;
	UInt32							unk58;
	UInt32							unk5C;
	hkVector4						kOrigin;
	hkArray<bhkEntityListener>		kListeners;
	hkArray<UInt32>					kUnk7C;
	float							unk88;
	float							unk8C;
	BSSimpleList<NiRefObjectPtr>	kActions;
	UInt32							unk98;
	UInt32							unk9C;

	void UpdateEx();
	void Unk_53Ex(NiAVObject* apObject0, NiAVObject* apObject1);

	static pfnRecFunc GetRecFunc(UInt32 aeType);

	static void DoObjectRec(NiAVObject* apObject, ObjectRecData& arData, pfnRecFunc apFunc);

	static bool SetMotion(NiAVObject* apObject, UInt32 aeType, bool abRecurse, bool abForce, bool abAllowActivate);
	static bool Activate(NiAVObject* apObject, bool abActivate, bool abRecurse, bool abForce);
	static bool UpdatePosition(NiAVObject* apObject, bool abRecurse, bool abForce);
};

ASSERT_SIZE(bhkWorld, 0xA0);