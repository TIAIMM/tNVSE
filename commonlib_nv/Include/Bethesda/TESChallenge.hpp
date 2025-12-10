#pragma once
#include "TESForm.hpp"
#include "TESDescription.hpp"
#include "TESScriptableForm.hpp"
#include "BGSMessageIcon.hpp"

class TESChallenge : public TESForm, public TESFullName, public TESDescription, public TESScriptableForm, public TESIcon, public BGSMessageIcon {
public:
	enum TESChallengeFlags {
		kChallenge_IsLocked = 0x1,
		kChallenge_IsCompleted = 0x2,
		kChallenge_NoRecur = 0x8,
	};

	struct ChallengeData {

		enum TESChallengeDataFlags {
			kChallengeData_StartDisabled = 0x1,
			kChallengeData_Recurring = 0x2,
			kChallengeData_ShowZeroProgress = 0x4,
		};

		UInt32 uiType;
		UInt32 uiThreshold;
		UInt32 uiFlags;
		UInt32 uiInterval;
		UInt16 usValue1;
		UInt16 usValue2;
		UInt16 usValue3;
	};

	ChallengeData	kData;
	UInt32			uiAmount;
	UInt32			uiChallengeFlags;
	TESForm*		SNAM;
	TESForm*		XNAM;
};
ASSERT_SIZE(TESChallenge, 0x7C);