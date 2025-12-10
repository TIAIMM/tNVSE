#pragma once

class BGSSaveLoadBuffer {
public:
	BGSSaveLoadBuffer();
	~BGSSaveLoadBuffer();

	char* pBuffer;

	void AllocateBuffer(UInt32 auiSize);
	void CopyBuffer(const char* apBuffer, UInt32 auiSize);
	void DeleteBuffer();
};

ASSERT_SIZE(BGSSaveLoadBuffer, 0x4);