#pragma once

#include "NiPixelData.hpp"
#include "NiTimeController.hpp"
#include "NiProperty.hpp"
#include "NiTexturingProperty.hpp"
#include "BSSimpleArray.hpp"
#include "BSString.hpp"
#include "NiColor.hpp"
#include "NiTexture.hpp"
#include "tList.h"

// C
//struct NiVector3
//{
//	float	x, y, z;
//};

struct NiPersistentSrcTextureRendererData : NiTexture::RendererData
{
	enum PlatformID : __int32
	{
		NI_ANY = 0x0,
		NI_XENON = 0x1,
		NI_PS3 = 0x2,
		NI_DX9 = 0x3,
		NI_NUM_PLATFORM_IDS = 0x4,
	};


	unsigned __int8* m_pucPixels;
	unsigned int* m_puiWidth;
	unsigned int* m_puiHeight;
	unsigned int* m_puiOffsetInBytes;
	unsigned int m_uiPadOffsetInBytes;
	unsigned int m_uiMipmapLevels;
	unsigned int m_uiPixelStride;
	unsigned int m_uiFaces;
	PlatformID m_eTargetPlatform;
	unsigned __int8* m_pucPristinePixels;
	unsigned int m_uiPristineMaxOffsetInBytes;
	unsigned int m_uiPristinePadOffsetInBytes;
	NiPointer<NiPalette> m_spPalette;
};
STATIC_ASSERT(sizeof(NiPersistentSrcTextureRendererData) == 0x94);

struct NiSourceTexture : NiTexture
{
	NiFixedString m_kFilename;
	NiFixedString m_kPlatformFilename;
	NiPointer<NiPersistentSrcTextureRendererData> m_spPersistentSrcRendererData;
	NiPointer<NiPixelData> m_spSrcPixelData;
	bool m_bStatic;
	bool m_bLoadDirectToRendererHint;
	bool m_bSrcRendererDataIsPersistent;
	char cLooked;
	NiFile* m_pFile;
};
STATIC_ASSERT(sizeof(NiSourceTexture) == 0x48);

// From OBSE
//struct FNTFile { // sizeof == 0x3928
//	struct GlyphInfo { // sizeof == 0x38
//		struct UV {
//			union { // x or u: percentage of width
//				float x;
//				float u;
//			};
//			union { // y or v: percentage of height
//				float y;
//				float v;
//			};
//		};
//		//
//		float unk00;       // 00
//		UV    topLeft;     // 04
//		UV    topRight;    // 0C
//		UV    bottomLeft;  // 14
//		UV    bottomRight; // 1C
//		float width;       // 24 // px
//		float height;      // 28 // px
//		float kerningLeft; // 2C // px // negative values pull adjacent letters closer
//		float kerningRight;// 30 // px // negative values pull adjacent letters closer
//		float ascent;      // 34 // px
//	};
//	//
//	float     fontSize;
//	SInt32    unk04;       // 0004 // Number of font textures. No more than 8 are allowed. See 00574892.
//	UInt32    unk08;
//	char      name[284];   // 000C // One texture filename (sans path) every 0x24 bytes, e.g. name[0], name[0x24], name[0x48], ...
//	GlyphInfo glyphs[256]; // 0128
//	//
//	// Question: what would multiple texture files accomplish? How would they even work? It seems 
//	// like the FontInfo struct only supports one texture file at a time in the first place.
//	//
//	// If you ensure that there is only one font texture, then you can have the name as long as 
//	// you like. TEX names aren't capped to 0x24 bytes; the game just starts reading at multiples 
//	// of 0x24 within the name string.
//};

// From JIP
//struct FontHeightData
//{
//	float heightBase;
//	float heightwGap;
//} s_fontHeightDatas[90];

//// From JIP
//// 54
//struct FontInfo
//{
//	// 38
//	struct GlyphInfo
//	{
//		struct UV {
//			union { // x or u: percentage of width
//				float x;
//				float u;
//			};
//			union { // y or v: percentage of height
//				float y;
//				float v;
//			};
//		};
//		//
//		float unk00;       // 00
//		UV    topLeft;     // 04
//		UV    topRight;    // 0C
//		UV    bottomLeft;  // 14
//		UV    bottomRight; // 1C
//		float width;
//		float height;
//		float kerningLeft;
//		float kerningRight;
//		float ascent;
//		//	totalWidth = width + widthMod
//	};
//
//	// 24
//	struct TexFileName
//	{
//		UInt32			textureID;
//		char			fileName[0x20];
//	};
//
//	// 3928
//	struct BufferData
//	{
//		float			lineHeight;				// 0000
//		UInt32			numTextures;			// 0004
//		TexFileName		textures[8];			// 0008
//		GlyphInfo		glyphs[256];	// 0128
//	};
//
//	struct ButtonIcon {
//		float unk01;
//		float unk02;
//		float unk03;
//		float unk04;
//		float unk05;
//		float unk06;
//		float unk07;
//		float unk08;
//	};
//
//	UInt8						isLoaded;	// 00
//	UInt8						pad01[3];	// 01
//	const char*						filePath;	// 04
//	UInt32						fontID;		// 08
//	NiTexturingProperty*		fontTexProp;	// 0C
//	UInt32						renderState[7];	// 10
//	float						maxCharHeight;		// 2C
//	float						maxWidthMod;		// 30
//	UInt32						unk34;		// 34
//	FontInfo::BufferData* fontData;// 38
//	UInt32						unk3C[2];	// 3C
//	BSSimpleArray<ButtonIcon>	buttonIconArray;		// 44
//
//	__forceinline FontInfo* Init(UInt32 fontID, const char* filePath, bool arg3)
//	{
//		return ThisStdCall<FontInfo*>(0xA12020, this, fontID, filePath, arg3);
//	}
//	__forceinline FontInfo* LoadFontIcon(const char* iconPath)
//	{
//		return ThisStdCall<FontInfo*>(0xA1AEE0, this, iconPath);
//	}
//};
//STATIC_ASSERT(sizeof(FontInfo) == 0x54);
//
////TempObject<UnorderedMap<const char*, FontInfo*>> s_fontInfosMap;

// From Aug 22, 2010 Fallout_Release_Beta.pdb
struct UVMap
{
	float fU;
	float fV;
};

STATIC_ASSERT(sizeof(UVMap) == 0x8);

struct FontLetter
{
	int iTextureIndex;
	UVMap pMapping[4];
	float fWidth;
	float fHeight;
	float fLeadingEdge;
	float fSpacing;
	float fTopEdge;
};

STATIC_ASSERT(sizeof(FontLetter) == 0x38);

struct TextureFile
{
	int iType;
	char pFilename[32];
};
STATIC_ASSERT(sizeof(TextureFile) == 0x24);

struct FontData
{
	float fBaseLine;
	int iTextureCount;
	TextureFile pTextureFiles[8];
	FontLetter pFontLetters[256];
};
STATIC_ASSERT(sizeof(FontData) == 0x3928);

struct  Font
{
	struct NiRect_float
	{
		float m_left;
		float m_right;
		float m_top;
		float m_bottom;
	};
	STATIC_ASSERT(sizeof(NiRect_float) == 0x10);

	struct ButtonIcon
	{
		float fWidth;
		float fXOffset;
		float fZOffset;
		float fSpacing;
		NiRect_float UVCoords;
	};
	STATIC_ASSERT(sizeof(ButtonIcon) == 0x20);

	unsigned __int16 iRefCount;
	char* pFontFile;
	int iFontNum;
	NiPointer<NiTexturingProperty> pTextureData[8];
	float fFontHeight;
	float fMaxDrop;
	int iLineOverlap;
	FontData* pFontData;
	BSStringT<char> IconAtlasTextureName;
	BSSimpleArray<ButtonIcon> ButtonIcons;

	struct TextData
	{
		BSStringT<char> xNewText;
		int iWidth;
		int iHeight;
		int iLineStart;
		int iLineEnd;
		int iCharCount;
		char cLineSep;
		BSSimpleList<int> xLineWidths;
	};

	__forceinline NiPoint3* AddIcon(int aiIconIndex, NiTriShape* apShape, NiPoint3* aPos)
	{
		return ThisStdCall<NiPoint3*>(0xA14650, this, aiIconIndex, apShape, aPos);
	}

	__forceinline Font* AddTextIcon(const char* astrIcon)
	{
		return ThisStdCall<Font*>(0xA1AEE0, this, astrIcon);
	}

	__forceinline NiTriShape* MakeTriShape(int aiChars, const NiColorA* axColor, bool abPrepareObject)
	{
		return ThisStdCall<NiTriShape*>(0xA14A20, this, aiChars, axColor, abPrepareObject);
	}

	__forceinline NiTriShape* MakeIconsTriShape()
	{
		return ThisStdCall<NiTriShape*>(0xA14DA0, this);
	}

};
STATIC_ASSERT(sizeof(Font) == 0x54);
STATIC_ASSERT(sizeof(Font::TextData) == 0x28);

// From JG and tweaked with pdb
// 164 (24)
class FontManager
{
public:
	FontManager();
	~FontManager();

	Font* pFont[8]; // 00
	bool bUseNewFonts; // 20
	UInt8 pad21[3]; // 21
	Font* extraFonts[80]; // 24

	//	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
	/*NiPoint3* GetStringDimensions(NiPoint3* outDims, const char* srcString, UInt32 fontID, UInt32 maxFlt = 0x7F7FFFFF,
		UInt32 startIdx = 0);*/

	__forceinline static FontManager* GetSingleton() { return *(FontManager**)0x11F33F8; }
};

// From JG
//0x11F33F8
// From Modern Minimap
//0x5BD5B0
//__declspec(naked) NiPoint3* FontManager::GetStringDimensions(NiPoint3* outDims, const char* srcString, UInt32 fontID,
//	UInt32 maxFlt, UInt32 startIdx)
//{
//	static const UInt32 procAddr = 0xA1B020;
//	__asm jmp procAddr
//}

//From Stewie Tweaks
//struct __declspec(align(4)) FontTextReplaced
//{
//	BSString str;
//	UInt32 wrapWidth;
//	UInt32 wrapLimit;
//	UInt32 initdToZero;
//	UInt32 wrapLines;
//	UInt32 length;
//	UInt8 newLineCharacter;
//	UInt8 gap1D[3];
//	tList<void> lineWidths;
//
//	FontTextReplaced()
//		initdToZero = 0;
//	{
//		wrapLines = 0;
//		length = 0;
//		newLineCharacter = 0;
//		lineWidths.Init();
//	};
//
//	~FontTextReplaced()
//	{
//		lineWidths.RemoveAll();
//	}
//
//	//BSStringT<T>::Set			0x4037F0 FontTextReplaced::StringSet
//	//BSStringT<T>::operator+=	0x404820 FontTextReplaced::StringAppend
//	//BSStringT<T>::Format		0x406F60 FontTextReplaced::StringFormat
//	//BSStringT<T>::ApplyFormat 0x406F90
//};

//STATIC_ASSERT(sizeof(FontTextReplaced) == 0x28);

// From Stewie Tweaks
class DebugText
{
public:
	virtual void    Unk_00(void);
	virtual void    Unk_01(UInt32 arg1, UInt32 arg2);
	virtual UInt32  Unk_02(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5, UInt32 arg6);
	virtual UInt32  Unk_03(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4);
	virtual void    Unk_04(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5, UInt32 arg6);
	virtual UInt32  Unk_05(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5);
	virtual void    Unk_06(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5);
	virtual UInt32  Unk_07(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5, UInt32 arg6, UInt32 arg7);
	virtual UInt32  Unk_08(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5);
	virtual UInt32  Unk_09(UInt32 arg1, UInt32 arg2, UInt32 arg3, UInt32 arg4, UInt32 arg5, UInt32 arg6);
	virtual UInt32  Unk_0A(UInt32 arg1);
	virtual void    Unk_0B(UInt32 arg1, UInt32 arg2);

	struct DebugLine
	{
		float           offsetX;    // 00
		float           offsetY;    // 04
		UInt32          alignment;  // 08
		NiNode* node;       // 0C
		BSString          text;       // 10
		float           flt18;      // 18    Always -1.0
		NiColorA    color;      // 1C
	};

	struct sPrintData
	{
		DebugLine* debugLine;
		UInt32 unk04; // set to 1 in 0xA0F9C2
		UInt32 unk08;
		UInt32 unk0C;
		float duration;
		UInt32 fontNumber;
	};

	struct TextNode
	{
		TextNode* next;		// 00
		TextNode* prev;		// 04
		BSString text;		// 08
	};

	struct TextList
	{
		TextNode* first;	// 00
		TextNode* last;		// 04
		unsigned int count;	// 08
	};

	enum TextAlign
	{
		kLeftAligned = 0x1,
		kCentered = 0x2,
	};

	DebugLine lines[200];				// 0004
	TextList textList;					// 2264
	BSSimpleArray<sPrintData*> lines2;	// 2270
	unsigned int unk2280;				// 2280
	unsigned int unk2284[3];			// 2284
	unsigned int unk2290[3];			// 2290

	static DebugText* GetSingleton() { return ((DebugText * (*)(bool))0xA0D9E0)(true); }
	// CreateLine 0xA0F8B0

	DebugText::DebugLine* GetDebugInputLine()
	{
		DebugText::DebugLine* linesPtr = this->lines;
		DebugText::DebugLine* result = linesPtr;
		float maxY = linesPtr->offsetY;
		UInt32 counter = 200;
		do
		{
			linesPtr++;
			if (!linesPtr->text.pString) break;
			if (maxY < linesPtr->offsetY)
			{
				maxY = linesPtr->offsetY;
				result = linesPtr;
			}
		} while (--counter);
		return result;
	}

	BSString* GetDebugInput()
	{
		return &(GetDebugInputLine()->text);
	}
};
STATIC_ASSERT(sizeof(DebugText) == 0x229C);