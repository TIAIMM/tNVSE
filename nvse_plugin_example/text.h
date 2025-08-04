#pragma once

#include "internal/netimmerse.h"

//From JGNVSE

struct FontHeightData
{
	float heightBase;
	float heightwGap;
} s_fontHeightDatas[90];

// 54
struct FontInfo
{
	struct BufferData
	{
		float lineHeight; // 0000
		UInt32 unk0004[73]; // 0004
		UInt32 unk0128[458]; // 0128
		float baseHeight; // 0850
		float flt0854; // 0854
		float flt0858; // 0858
	};

	struct ButtonIcon;

	UInt8 isLoaded; // 00
	UInt8 pad01[3]; // 01
	char* filePath; // 04
	UInt8 fontID; // 08
	UInt8 pad09[3]; // 09
	NiTexturingProperty* texProp; // 0C
	UInt32 unk10[7]; // 10
	float flt2C; // 2C
	float flt30; // 30
	UInt32 unk34; // 34
	BufferData* bufferData; // 38
	UInt32 unk3C[2]; // 3C
	BSSimpleArray<ButtonIcon> arr44; // 44
};

STATIC_ASSERT(sizeof(FontInfo) == 0x54);

// 164 (24)
class FontManager
{
public:
	FontManager();
	~FontManager();

	FontInfo* fontInfos[8]; // 00
	UInt8 byte20; // 20
	UInt8 pad21[3]; // 21
	FontInfo* extraFonts[80]; // 24

	//	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
	NiVector3* GetStringDimensions(NiVector3* outDims, const char* srcString, UInt32 fontID, UInt32 maxFlt = 0x7F7FFFFF,
		UInt32 startIdx = 0);
};

__declspec(naked) NiVector3* FontManager::GetStringDimensions(NiVector3* outDims, const char* srcString, UInt32 fontID,
	UInt32 maxFlt, UInt32 startIdx)
{
	static const UInt32 procAddr = 0xA1B020;
	__asm jmp procAddr
}
