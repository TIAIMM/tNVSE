#pragma once

#include "NiMemObject.hpp"

class NiStream;

class NiAnimationKey : public NiMemObject {
public:
	NiAnimationKey() : m_fTime(0) {};
	~NiAnimationKey() {}

	enum KeyContent {
		FLOATKEY,
		POSKEY,
		ROTKEY,
		COLORKEY,
		TEXTKEY,
		BOOLKEY,
		NUMKEYCONTENTS
	};

	enum KeyType {
		NOINTERP,
		LINKEY,
		BEZKEY,
		TCBKEY,
		EULERKEY,
		STEPKEY,
		NUMKEYTYPES
	};

	float m_fTime;

	typedef void (*InterpFunction)(float afTime, const NiAnimationKey* apKey0, const NiAnimationKey* apKey1, void* apResult);

	void SetTime(float afTime);
	float GetTime() const;

	static UInt8* const ms_keysizes;
	static UInt8 GetKeySize(KeyContent eContent, KeyType eType);
	static void SaveBinary(NiStream& arStream, NiAnimationKey* apKey);
	static const char* KeyTypeToString(KeyType key);

	NiAnimationKey* GetKeyAt(UInt32 uiIndex, UInt8 ucKeySize) const;
};

ASSERT_SIZE(NiAnimationKey, 0x4);