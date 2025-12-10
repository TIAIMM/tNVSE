#pragma once

class TESForm;
class TESObjectREFR;
class Actor;

#ifdef GetForm
#undef GetForm
#endif

#ifdef LoadString
#undef LoadString
#endif

class BGSSaveLoadFile;

class BGSLoadGameBuffer {
public:
	BGSLoadGameBuffer();
	~BGSLoadGameBuffer();

	virtual UInt8			GetVersion() const;
	virtual TESForm*		GetForm() const;
	virtual TESObjectREFR*	GetReference() const;
	virtual Actor*			GetActor() const;

	char*	pBuffer;
	UInt32	uiBufferSize;
	UInt32	uiBufferPosition;

	SInt32 Load(BGSSaveLoadFile* apFile);
	void LoadDataEndian(char* apData, UInt32 auiSize);
	template <typename T>
	void LoadDataEndian(T* apData) {
		LoadDataEndian((char*)apData, sizeof(T));
	}
	char* LoadString(char* apText);
};

ASSERT_SIZE(BGSLoadGameBuffer, 0x10);