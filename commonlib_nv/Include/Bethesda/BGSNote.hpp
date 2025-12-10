#pragma once

#include "TESBoundObject.hpp"
#include "TESModel.hpp"
#include "TESIcon.hpp"
#include "BGSPickupPutdownSounds.hpp"

class TESDescription;
class TESNPC;
class TESTopic;

class BGSNote : public TESBoundObject, public TESModel, public TESFullName, public TESIcon, public BGSPickupPutdownSounds {
public:
	BGSNote();
	~BGSNote();

	union {
		TESDescription* pNoteText;
		TESTexture*		pPicture;
		TESTopic*		pTopic;
	};
	TESNPC*			pVoiceNPC;
	UInt32			unk74;
	UInt32			unk78;
	UInt8			ucNoteType;
	bool			bHasBeenRead;
	UInt8			byte7E;
	UInt8			byte7F;
};

ASSERT_SIZE(BGSNote, 0x80u);