#pragma once

#include "BSSimpleArray.hpp"
#include "BSSimpleList.hpp"
#include "BSString.hpp"
#include "ITypes.h"
#include "Memory.h"
#include "GameRTTI.h"
#include "Menu.hpp"
#include "NiColor.hpp"
#include "NiNode.hpp"
#include "NiPersistentSrcTextureRendererData.hpp"
#include "NiPoint3.hpp"
#include "NiProperty.hpp"
#include "NiSourceTexture.hpp"
#include "NiTPointerList.hpp"
#include "NiTexture.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTimeController.hpp"
#include "NiTriShape.hpp"
#include <cstdint>
#include <limits>

class TESNPC;

// C
//struct NiVector3
//{
//	float	x, y, z;
//};

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

struct  Font
{
	using NiRect_float = ::NiRect_float;
	using ButtonIcon = ::ButtonIcon;

	UInt16 iRefCount;
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

	__forceinline static void __cdecl ConvertCharacter(UInt8& arChar)
	{
		CdeclCall(0xA122B0, &arChar);
	}

	__forceinline void AddChar(FontLetter* apLetter, int aiVert, NiTriShape* apShape, NiPoint3* apPosition, const NiColorA* apColor)
	{
		ThisStdCall(0xA142D0, this, apLetter, aiVert, apShape, apPosition, apColor);
	}

};
STATIC_ASSERT(sizeof(Font) == 0x54);
STATIC_ASSERT(sizeof(Font::TextData) == 0x28);

// PC TextEditMenu path. Xbox Release Beta uses XVirtualKeyboard for
// PlayerNameEntryMenu, so the TextEditState/TextEditMenu layout is recovered
// from the PC 1.4.0.525 executable. PlayerNameEntryMenu names are from the
// Aug 22, 2010 Xbox Release Beta PDB.
enum TextEditInputCode : SInt32
{
	kTextEditInput_Backspace = -2147483647 - 1,
	kTextEditInput_Left = -2147483647,
	kTextEditInput_Right = -2147483646,
	kTextEditInput_Home = -2147483643,
	kTextEditInput_End = -2147483642,
	kTextEditInput_Delete = -2147483641,
	kTextEditInput_Confirm = -2147483640,
};

struct TextEditState
{
	BSStringT<char> xText;        // 00: raw edit buffer
	BSStringT<char> xDisplayText; // 08: buffer returned by BuildDisplayText
	UInt32 iCaretByteOffset;      // 10
	SInt32 iMaxPixelWidth;        // 14: -1 disables width validation
	UInt32 iFontIndex;            // 18
	UInt32 uiCaretBlinkTime;      // 1C
	bool bCaretVisible;           // 20
	bool bActive;                 // 21
	bool bClearOnNextType;        // 22
	UInt8 pad23;                  // 23

	__forceinline TextEditState* Init()
	{
		return ThisStdCall<TextEditState*>(0x716980, this);
	}

	__forceinline void SetText(const char* apText)
	{
		ThisStdCall(0x716A70, this, apText);
	}

	__forceinline UInt32 SetMaxPixelWidth(UInt32 auiTileWidth)
	{
		return ThisStdCall<UInt32>(0x716AA0, this, auiTileWidth);
	}

	__forceinline bool IsActive() const
	{
		return ThisStdCall<bool>(0x716AE0, this);
	}

	// 0x716B00 is ECX=this, EDX=aiKey, stack=aiChar; it is not a normal
	// thiscall wrapper.
	__forceinline void InputUnk01(SInt32 aiKey, SInt32 aiChar)
	{
		reinterpret_cast<void(__fastcall*)(TextEditState*, SInt32, SInt32)>(0x716B00)(this, aiKey, aiChar);
	}

	__forceinline void SetActive(bool abActive)
	{
		ThisStdCall(0x717010, this, abActive);
	}

	__forceinline const char* BuildDisplayText()
	{
		return ThisStdCall<const char*>(0x7170A0, this);
	}

	__forceinline bool FitsMaxPixelWidth(const char* apText)
	{
		return ThisStdCall<bool>(0x717230, this, apText);
	}

	__forceinline void SetClearOnNextType(bool abClear)
	{
		ThisStdCall(0x7E6580, this, abClear);
	}
};
STATIC_ASSERT(sizeof(TextEditState) == 0x24);

class TextEditMenu : public Menu
{
public:
	static inline auto bs_rtti = RTTI_TextEditMenu;

	using ValidateTextCallback = bool(__cdecl*)(const char* apText);

	Tile* pEditText;                      // 28
	Tile* pOkButton;                      // 2C
	Tile* pTitle;                         // 30
	TextEditState xEditState;             // 34
	ValidateTextCallback pValidateText;   // 58

	__forceinline static TextEditMenu* GetCurrent()
	{
		return *reinterpret_cast<TextEditMenu**>(0x11DAEC4);
	}

	__forceinline static bool __cdecl Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText)
	{
		return CdeclCall<bool>(0x7E6320, apTitle, apInitialText, apValidateText);
	}

	__forceinline bool HandleKeyboardInput(SInt32 aiInput)
	{
		return ThisStdCall<bool>(0x7E6620, this, aiInput);
	}

	__forceinline void Refresh()
	{
		ThisStdCall(0x7E6700, this);
	}
};
STATIC_ASSERT(sizeof(TextEditMenu) == 0x5C);

class PlayerNameEntryMenu : public Menu
{
public:
	static inline auto bs_rtti = RTTI_PlayerNameEntryMenu;

	TESNPC* pPlayerBase; // 28

	__forceinline static bool __cdecl IsValidName(const char* apText)
	{
		return CdeclCall<bool>(0x7AB820, apText);
	}

	__forceinline void Finish(const char* apName)
	{
		ThisStdCall(0x7AB9A0, this, apName);
	}
};
STATIC_ASSERT(sizeof(PlayerNameEntryMenu) == 0x2C);

// From JG and tweaked with pdb
// 164 (24)
class FontManager
{
public:
	static constexpr UInt32 kVanillaFontCount = 8;
	static constexpr UInt32 kStockRichTextFontCount = 8;
	static constexpr UInt32 kJipExtendedFontCount = 80;

	// Rich-text path used by FontManager::PrepText (0xA18A30),
	// FontManager::PrepHypertext (0xA17390), and TextDoc::Render (0xA19060).
	// Field names are from the Aug 22, 2010 Xbox Release Beta PDB; offsets are
	// verified against the PC 1.4.0.525 executable.
	struct TextData
	{
		int iDefaultFont;        // 00
		int iJustification;      // 04: left=1, center=2, right=4
		NiColorA xColor;         // 08
		char cLineSep;           // 18
		UInt8 pad19[3];          // 19
		int iWidth;              // 1C
		int iHeight;             // 20
		int iPageNum;            // 24
		int iLines;              // 28
		int iNumPages;           // 2C
		int iNumLines;           // 30
		bool bIsHypertext;       // 34
		UInt8 pad35[3];          // 35
		BSStringT<char> xNewText;// 38
	};

	struct CharData
	{
		int iFontIndex;          // 00
		UInt8 cChar;             // 04
		UInt8 pad05[3];          // 05
		NiColorA xColor;         // 08
		int iJustification;      // 18
		BSStringT<char> xFilename;// 1C: non-empty means IMG/SRC image entry
		int iWidth;              // 24
		int iRise;               // 28
		int iDrop;               // 2C
		int iLeadingEdge;        // 30
		int iX;                  // 34

		__forceinline void SetChar(UInt8 acChar)
		{
			ThisStdCall(0xA1B7F0, this, acChar);
		}

		__forceinline CharData* Copy()
		{
			return ThisStdCall<CharData*>(0xA1B660, this);
		}

		__forceinline void RevertToDefault()
		{
			ThisStdCall(0xA1B770, this);
		}
	};

	struct TextDoc;
	struct TextPage;
	struct TextLine
	{
		NiTPointerList<CharData*> xChars; // 00
		int iWidth;                       // 0C
		int iNextSpacing;                 // 10
		int iRise;                        // 14
		int iDrop;                        // 18
		int iSkippedSpace;                // 1C
		int iJustify;                     // 20
		int iPageWidth;                   // 24
		UInt32 unk28;                     // 28
		TextPage* pPage;                  // 2C

		__forceinline TextLine* AddChar(CharData* apChar, bool abAddHead)
		{
			return ThisStdCall<TextLine*>(0xA19F70, this, apChar, abAddHead);
		}
	};

	struct TextPage
	{
		NiTPointerList<TextLine*> xLines; // 00
		int iWidth;                       // 0C: widest line on page
		int iHeight;                      // 10
		int iPageWidth;                   // 14
		int iPageHeight;                  // 18
		int iLastFontHeight;              // 1C
		int pCharsPerFont[kStockRichTextFontCount]; // 20
		TextDoc* pDoc;                    // 40

		__forceinline int GetCharCountForFont(int aiFont)
		{
			return ThisStdCall<int>(0xA19B00, this, aiFont);
		}

		__forceinline TextPage* AddChar(CharData* apChar, int aiNewLines)
		{
			return ThisStdCall<TextPage*>(0xA19C00, this, apChar, aiNewLines);
		}
	};

	struct TextDoc
	{
		NiTPointerList<TextPage*> xPages; // 00
		int iPageWidth;                   // 0C
		int iPageHeight;                  // 10
		int iPageNum;                     // 14

		__forceinline TextPage* CurrentPage()
		{
			return ThisStdCall<TextPage*>(0xA18FF0, this);
		}

		__forceinline void AddChar(CharData* apChar, int aiNewLines, bool abNewPage)
		{
			ThisStdCall(0xA19A10, this, apChar, aiNewLines, abNewPage);
		}

		__forceinline void Render(NiNode* apNode, TextData* apData)
		{
			ThisStdCall(0xA19060, this, apNode, apData);
		}

		__forceinline void Destroy()
		{
			ThisStdCall(0xA1B990, this);
		}
	};

	FontManager();
	~FontManager();

	Font* pFont[kVanillaFontCount]; // 00
	bool bUseNewFonts; // 20
	UInt8 pad21[3]; // 21
	Font* extraFonts[kJipExtendedFontCount]; // 24

	//	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
	/*NiPoint3* GetStringDimensions(NiPoint3* outDims, const char* srcString, UInt32 fontID, UInt32 maxFlt = 0x7F7FFFFF,
		UInt32 startIdx = 0);*/

	__forceinline static FontManager* GetSingleton()
	{
		return *(FontManager**)0x11F33F8;
	}

	__forceinline static Float32 __stdcall GetLinePadding(UInt32 fontID)
	{
		return StdCall<Float32>(0xA1B3A0, fontID);
	}

	__forceinline static int __stdcall GetCharType(char acChar)
	{
		return StdCall<int>(0xA16DA0, acChar);
	}

	__forceinline TextDoc* PrepHypertext(BSStringT<char>& arTextString, TextData& arData)
	{
		return ThisStdCall<TextDoc*>(0xA17390, this, &arTextString, &arData);
	}

	__forceinline TextDoc* PrepText(BSStringT<char>& arTextString, TextData& arData)
	{
		return ThisStdCall<TextDoc*>(0xA18A30, this, &arTextString, &arData);
	}
};

STATIC_ASSERT(sizeof(FontManager) == 0x164);

STATIC_ASSERT(sizeof(FontManager::TextData) == 0x40);
STATIC_ASSERT(sizeof(FontManager::CharData) == 0x38);
STATIC_ASSERT(sizeof(FontManager::TextLine) == 0x30);
STATIC_ASSERT(sizeof(FontManager::TextPage) == 0x44);
STATIC_ASSERT(sizeof(FontManager::TextDoc) == 0x18);

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

	static DebugText* GetSingleton()
	{
		return ((DebugText * (*)(bool))0xA0D9E0)(true);
	}
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
