#pragma once

#include "BGSSaveLoadBuffer.hpp"

class TESForm;
class TESObjectREFR;
class Actor;
class BGSSaveLoadFile;

class BGSSaveGameBuffer {
public:
	BGSSaveGameBuffer();
	~BGSSaveGameBuffer();

	virtual TESForm*		GetForm() const;
	virtual TESObjectREFR*	GetReference() const;
	virtual Actor*			GetActor() const;

	BGSSaveLoadBuffer	kBuffer;
	UInt32				uiBufferSize;
	UInt32				uiBufferPosition;
	UInt32				uiWriteCount;

	void SaveData(const void* apData, UInt32 auiSize, UInt32 unused = 0);
	template <typename T>
	void SaveData(const T& aData) {
		SaveData(&aData, sizeof(T));
	}

	void SaveString(const char* apData, UInt32 auiSize);

	void Save(BGSSaveLoadFile* apFile);
};

ASSERT_SIZE(BGSSaveGameBuffer, 0x14);