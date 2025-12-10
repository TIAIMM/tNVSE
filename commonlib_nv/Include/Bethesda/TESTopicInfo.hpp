#pragma once

#include "TESForm.hpp"
#include "TESCondition.hpp"
#include "NiTArray.hpp"

class TESTopic;

class TESTopicInfo : public TESForm, public TESCondition {
public:
	TESTopicInfo();
	~TESTopicInfo();

	struct RelatedTopics
	{
		BSSimpleList<TESTopic*>	linkFrom;
		BSSimpleList<TESTopic*>	choices;
		BSSimpleList<TESTopic*>	unknown;
	};

	UInt16					unk20;			
	bool					bSaidOnce;		
	UInt8					ucType;			
	UInt8					nextSpeaker;	
	UInt8					flags1;			
	UInt8					flags2;						
	BSString				strPrompt;			
	BSSimpleList<TESTopic*>	kAddTopics;		
	RelatedTopics*			pRelatedTopics;	
	UInt32					speaker;		
	UInt32					actorValueOrPerk;
	UInt32					speechChallenge;
	TESQuest*				pQuest;
	UInt32					modInfoFileOffset;
	TESTopic*				pParentTopic; // Added by JIP
};

ASSERT_SIZE(TESTopicInfo, 0x50 + 4);