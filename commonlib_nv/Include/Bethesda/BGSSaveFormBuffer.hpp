#pragma once

#include "BGSSaveGameBuffer.hpp"
#include "BGSSaveLoadFormHeader.hpp"

class BGSSaveFormBuffer : public BGSSaveGameBuffer {
public:
	BGSSaveFormBuffer();
	~BGSSaveFormBuffer();

	BGSSaveLoadFormHeader	kHeader;
	TESForm*				pForm;
};

ASSERT_SIZE(BGSSaveFormBuffer, 0x24);