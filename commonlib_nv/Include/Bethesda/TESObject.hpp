#pragma once

#include "TESForm.hpp"

class NiNode;
class TESObjectREFR;
class TESBoundObject;
class TESWaterForm;
class TESObjectList;

class TESObject : public TESForm {
public:
	static inline auto bs_rtti = RTTI_TESObject;

	TESObject();
	virtual ~TESObject();

	virtual UInt32			Unk_4E();
	virtual bool			Unk_4F();
	virtual TESWaterForm*	GetWaterType();
	virtual bool			IsAutoCalc() const;
	virtual void			SetAutoCalc(bool);
	virtual NiNode*			Clone3D(TESObjectREFR* apRequester, bool abDeepCopy);
	virtual void			UnClone3D(TESObjectREFR* apRequester);
	virtual bool			IsMarker();
	virtual bool			IsOcclusionMarker();
	virtual void			Unk_57();
	virtual bool			ReloadModel();
	virtual bool			Unk_59(void* arg);
	virtual void			Unk_5A(void* arg0, void* arg1);
	virtual UInt32			Unk_5B();
	virtual UInt32			Unk_5C();
	virtual NiNode*			LoadGraphics(TESObjectREFR* apRef);

	TESObjectList*	pList;
	TESObject*		pPrev;
	TESObject*		pNext;

	TESObject* GetNext() const;
	TESObject* GetPrev() const;

	const char* GetModelFileName(TESObjectREFR* apRef = nullptr) const;
};

ASSERT_SIZE(TESObject, 0x24)