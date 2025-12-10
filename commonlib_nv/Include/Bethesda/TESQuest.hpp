#pragma once

#include "TESForm.hpp"
#include "TESScriptableForm.hpp"
#include "TESIcon.hpp"
#include "TESCondition.hpp"

class BGSQuestObjective;
class ScriptLocals;

struct VariableInfo;

class TESQuest : public TESForm, public TESScriptableForm, public TESIcon, public TESFullName {
public:
	static inline auto bs_rtti = RTTI_TESQuest;

	TESQuest();
	~TESQuest();

	virtual char* GetEditorName() const;

	struct StageInfo {
		UInt8				ucStage;
		UInt8				ucStatus;
		BSSimpleList<void*>	kLogEntries;
	};

	union LocalVariableOrObjectivePtr {
		BGSQuestObjective*	pObjective;
		VariableInfo*		pVarInfoIndex;
	};

	struct Data {
		UInt8	ucFlags;
		UInt8	ucPriority;
		float	fQuestDelayTime;
	};

	Data										kData;
	BSSimpleList<StageInfo*>					kStages;
	BSSimpleList<LocalVariableOrObjectivePtr>	kVarOrObjectives;
	TESCondition								kConditions;
	ScriptLocals*								pScriptLocals;
	UInt8										ucCurrentStage;
	BSString									strEditorID;

	bool IsRunning() const {
		return ThisStdCall<bool>(0x455620, this);
	}

	void ResetQuest() const {
		ThisStdCall(0x60D720, this);
	}
};

ASSERT_SIZE(TESQuest, 0x6C);