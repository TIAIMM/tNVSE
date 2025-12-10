#pragma once

#include "BGSLoadGameBuffer.hpp"
#include "BGSSaveLoadFormHeader.hpp"

class BGSFormChange;

class BGSLoadFormBuffer : public BGSLoadGameBuffer {
public:
	BGSLoadFormBuffer();
	~BGSLoadFormBuffer();

	UInt32					uiFormID;
	BGSSaveLoadFormHeader	kHeader;
	UInt32					uiBufferSize;
	TESForm*				pForm;
	UInt32					uiFlags28;
	BGSFormChange*			pCurrentFormChange;
};

ASSERT_SIZE(BGSLoadFormBuffer, 0x30);