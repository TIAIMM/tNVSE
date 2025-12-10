#pragma once

#include "BGSSaveLoadBuffer.hpp"

class BGSSaveFormBuffer;

class BGSLoadGameSubBuffer {
public:
	BGSLoadGameSubBuffer();
	BGSLoadGameSubBuffer(BGSLoadGameSubBuffer* apBuffer);
	BGSLoadGameSubBuffer(BGSSaveFormBuffer& arBuffer);
	~BGSLoadGameSubBuffer();

	BGSSaveLoadBuffer kBuffer;

	void AllocateBuffer(UInt32 auiSize);
	void CopyBuffer(BGSSaveFormBuffer* apBuffer);
	void DeleteBuffer();
};

ASSERT_SIZE(BGSLoadGameSubBuffer, 0x4);