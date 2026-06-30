#include "BGSLoadGameBuffer.hpp"
#include "BGSSaveLoadGame.hpp"

// GAME - 0x8646B0
BGSLoadGameBuffer::BGSLoadGameBuffer() {
	pBuffer				= nullptr;
	uiBufferSize		= 0;
	uiBufferPosition	= 0;
}

// GAME - 0x8646F0
BGSLoadGameBuffer::~BGSLoadGameBuffer() {
	if (pBuffer) {
		delete[] pBuffer;
		pBuffer = nullptr;
	}
}

// GAME - 0x864720
UInt8 BGSLoadGameBuffer::GetVersion() const {
	return BGSSaveLoadGame::GetSingleton()->ucCurrentMinorVersion;
}

TESForm* BGSLoadGameBuffer::GetForm() const {
	return nullptr;
}

TESObjectREFR* BGSLoadGameBuffer::GetReference() const {
	return nullptr;
}

Actor* BGSLoadGameBuffer::GetActor() const {
	return nullptr;
}

// GAME - 0x864740
SInt32 BGSLoadGameBuffer::Load(BGSSaveLoadFile* apFile) {
	return ThisStdCall<SInt32>(0x864740, this, apFile);
}

// GAME - 0x864980
void BGSLoadGameBuffer::LoadDataEndian(char* apData, UInt32 auiSize) {
	ThisStdCall(0x864980, this, apData, auiSize);
}

// GAME - 0x8649A0
char* BGSLoadGameBuffer::LoadString(char* apText) {
	return ThisStdCall<char*>(0x8649A0, this, apText);
}
