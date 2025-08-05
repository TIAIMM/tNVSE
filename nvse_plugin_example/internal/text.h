#pragma once

#include "internal/netimmerse.h"

//From JIP, JGNVSE, Stewie, Wall & Confused

// From JIP
struct FontHeightData
{
	float heightBase;
	float heightwGap;
} s_fontHeightDatas[90];

// From JIP
// 54
struct FontInfo
{
	// 38
	struct CharDimensions
	{
		float			flt00[9];
		float			width;		// 24
		float			height;		// 28
		float			flt2C;		// 2C
		float			widthMod;	// 30
		float			flt34;		// 34
		//	totalWidth = width + widthMod
	};

	// 24
	struct TexFileName
	{
		UInt32			unk00;
		char			fileName[0x20];
	};

	// 3928
	struct BufferData
	{
		float			lineHeight;				// 0000
		UInt32			numTextures;			// 0004
		TexFileName		textures[8];			// 0008
		CharDimensions	charDimensions[256];	// 0128
	};

	struct ButtonIcon;

	UInt8						isLoaded;	// 00
	UInt8						pad01[3];	// 01
	char* filePath;	// 04
	UInt32						fontID;		// 08
	NiTexturingProperty* texProp;	// 0C
	UInt32						unk10[7];	// 10
	float						flt2C;		// 2C
	float						flt30;		// 30
	UInt32						unk34;		// 34
	BufferData* bufferData;// 38
	UInt32						unk3C[2];	// 3C
	BSSimpleArray<ButtonIcon>	arr44;		// 44

	__forceinline FontInfo* Init(UInt32 fontID, const char* filePath, bool arg3)
	{
		return ThisStdCall<FontInfo*>(0xA12020, this, fontID, filePath, arg3);
	}
};

STATIC_ASSERT(sizeof(FontInfo) == 0x54);

// From JG
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

	__forceinline static FontManager* GetSingleton();
};

__declspec(naked) NiVector3* FontManager::GetStringDimensions(NiVector3* outDims, const char* srcString, UInt32 fontID,
	UInt32 maxFlt, UInt32 startIdx)
{
	static const UInt32 procAddr = 0xA1B020;
	__asm jmp procAddr
}

//From Stewie Tweaks
struct __declspec(align(4)) FontTextReplaced
{
	String str;
	UInt32 wrapWidth;
	UInt32 wrapLimit;
	UInt32 initdToZero;
	UInt32 wrapLines;
	UInt32 length;
	UInt8 newLineCharacter;
	UInt8 gap1D[3];
	tList<void> lineWidths;

	FontTextReplaced()
	{
		str.m_bufLen = 0;
		str.m_data = 0;
		str.m_dataLen = 0;
		wrapWidth = 0;
		wrapLimit = 0;
		initdToZero = 0;
		wrapLines = 0;
		length = 0;
		newLineCharacter = 0;
		lineWidths.Init();
	};

	~FontTextReplaced()
	{
		str.Set(NULL);
		lineWidths.RemoveAll();
	}

	void GetVariableEscapedText(const char* input);
};

STATIC_ASSERT(sizeof(FontTextReplaced) == 0x28);
static void(__thiscall* Font__CheckForVariablesInText)(FontInfo*, const char* input, FontTextReplaced* a3) = (void(__thiscall*)(FontInfo*, const char*, FontTextReplaced*))0xA12FB0;