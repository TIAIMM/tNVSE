#pragma once

#include "BSString.hpp"
#include "TESCondition.hpp"
#include "MagicSystem.hpp"

class Actor;
class EffectSetting;

struct  EffectItemData {
	UInt32				uiMagnitude;
	UInt32				uiArea;
	UInt32				uiDuration;
	MagicSystem::Range	eRange;
	UInt32				uiActorValue;
};

class EffectItem {
public:
	EffectItem();
	~EffectItem();

	struct ScriptEffectInfo {
		UInt32		uiScriptRefID;
		UInt32		uiSchool;
		BSString	strEffectName;
		UInt32		uiVisualEffectCode;
		UInt32		bIsHostile;

		void SetName(const char* name);
		void SetSchool(UInt32 school);
		void SetVisualEffectCode(UInt32 code);
		void SetIsHostile(bool bIsHostile);
		bool IsHostile() const;
		void SetScriptRefID(UInt32 refID);

		ScriptEffectInfo* Clone() const;
		void CopyFrom(const ScriptEffectInfo* from);
		static ScriptEffectInfo* Create();
	};

	EffectItemData	kData;
	EffectSetting*	pEffectSetting;
	float			fRawCost;
	TESCondition	kConditions;
};

ASSERT_SIZE(EffectItem, 0x24);