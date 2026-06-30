#include "BGSSaveGameBuffer.hpp"

// GAME - 0x865BD0
BGSSaveGameBuffer::BGSSaveGameBuffer() {
	uiBufferSize = 0;
	uiBufferPosition = 0;
	uiWriteCount = 0;
}

// GAME - 0x865C10
BGSSaveGameBuffer::~BGSSaveGameBuffer() {
}

TESForm* BGSSaveGameBuffer::GetForm() const {
	return nullptr;
}

TESObjectREFR* BGSSaveGameBuffer::GetReference() const {
	return nullptr;
}

Actor* BGSSaveGameBuffer::GetActor() const {
	return nullptr;
}

// GAME - 0x865E50
void BGSSaveGameBuffer::SaveData(const void* apData, UInt32 auiSize, UInt32 unused) {
	ThisStdCall(0x865E50, this, apData, auiSize, unused);
}

// GAME - 0x865E70
void BGSSaveGameBuffer::SaveString(const char* apData, UInt32 auiSize) {
	ThisStdCall(0x865E70, this, apData, auiSize);
}

// GAME - 0x865C40
void BGSSaveGameBuffer::Save(BGSSaveLoadFile* apFile) {
	ThisStdCall(0x865C40, this, apFile);
}
