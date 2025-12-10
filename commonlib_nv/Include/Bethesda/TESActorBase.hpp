#pragma once

#include "TESBoundAnimObject.hpp"
#include "TESActorBaseData.hpp"
#include "TESContainer.hpp"
#include "BGSTouchSpellForm.hpp"
#include "TESSpellList.hpp"
#include "TESAIForm.hpp"
#include "TESHealthForm.hpp"
#include "TESAttributes.hpp"
#include "TESAnimation.hpp"
#include "TESFullName.hpp"
#include "TESModel.hpp"
#include "BGSDestructibleObjectForm.hpp"
#include "TESScriptableForm.hpp"
#include "ActorValueOwner.hpp"

class BGSBodyPartData;
class TESCombatStyle;

class TESActorBase :
	public TESBoundAnimObject, public TESActorBaseData, public TESContainer,
	public BGSTouchSpellForm, public TESSpellList, public TESAIForm, public TESHealthForm, public TESAttributes,
	public TESAnimation, public TESFullName, public TESModel, public TESScriptableForm, public ActorValueOwner, public BGSDestructibleObjectForm {
public:
	static inline auto bs_rtti = RTTI_TESActorBase;

	TESActorBase();
	virtual ~TESActorBase();

	virtual BGSBodyPartData*	GetBodyPartData() const;
	virtual void				SetBodyPartData(BGSBodyPartData* apData);
	virtual TESCombatStyle*		GetCombatStyle() const;
	virtual void				SetCombatStyle(TESCombatStyle* apCombatStyle);
	virtual void				SetActorValueF(UInt32 aeValueCode, float afValue);	// calls Fn65
	virtual void				SetActorValueI(UInt32 aeValueCode, SInt32 aiValue);
	virtual void				ModActorValueF(UInt32 aeValueCode, float afValue);
	virtual void				ModActorValueI(UInt32 aeValueCode, SInt32 aiValue);	// mod actor value?
};

ASSERT_SIZE(TESActorBase, 0x10C);