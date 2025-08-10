#pragma once

#include "Utilities.h"
#include "NiNodes.h"

class NiMultiTargetTransformController;

//From JIP, JGNVSE, Stewie, Wall & Confused

//From JG

// 0C
class NiExtraData : public NiObject {
public:
	NiExtraData();
	~NiExtraData();

	virtual void	Unk_23(void);
	virtual void	Unk_24(void);

	UInt32			unk08;		// 08
};

// 34
class NiTimeController : public NiObject {
public:
	NiTimeController();
	~NiTimeController();

	virtual void	Unk_23(void);
	virtual void	Unk_24(void);
	virtual void	Unk_25(void);
	virtual void	SetTarget(NiNode* pTarget);
	virtual void	Unk_27(void);
	virtual void	Unk_28(void);
	virtual void	Unk_29(void);
	virtual void	Unk_2A(void);
	virtual void	Unk_2B(void);
	virtual void	Unk_2C(void);

	UInt16								flags;				// 08
	UInt16								unk0A;				// 0A
	float								frequency;			// 0C
	float								phaseTime;			// 10
	float								flt14;				// 14
	float								flt18;				// 18
	float								flt1C;				// 1C
	float								flt20;				// 20
	float								flt24;				// 24
	float								flt28;				// 28
	NiNode* target;			// 2C
	NiMultiTargetTransformController* multiTargetCtrl;	// 30
};

// 18
class NiProperty : public NiObjectNET {
public:
	NiProperty();
	~NiProperty();

	virtual UInt32	GetPropertyType();
	virtual void	UpdateController(float arg);

	enum {
		kPropertyType_Alpha = 0,
		kPropertyType_Culling = 1,
		kPropertyType_Material = 2,
		kPropertyType_Shade = 3,
		kPropertyType_TileShader = kPropertyType_Shade,
		kPropertyType_Stencil = 4,
		kPropertyType_Texturing = 5,
		kPropertyType_Dither = 8,
		kPropertyType_Specular = 9,
		kPropertyType_VertexColor = 10,
		kPropertyType_ZBuffer = 11,
		kPropertyType_Fog = 13,
	};
};

// 30
class NiTexturingProperty : public NiProperty {
public:
	NiTexturingProperty();
	~NiTexturingProperty();

	UInt32				unk18[6];	// 18
};

// From OBSE
struct FNTFile { // sizeof == 0x3928
	struct GlyphInfo { // sizeof == 0x38
		struct UV {
			union { // x or u: percentage of width
				float x;
				float u;
			};
			union { // y or v: percentage of height
				float y;
				float v;
			};
		};
		//
		float unk00;       // 00
		UV    topLeft;     // 04
		UV    topRight;    // 0C
		UV    bottomLeft;  // 14
		UV    bottomRight; // 1C
		float width;       // 24 // px
		float height;      // 28 // px
		float kerningLeft; // 2C // px // negative values pull adjacent letters closer
		float kerningRight;// 30 // px // negative values pull adjacent letters closer
		float ascent;      // 34 // px
	};
	//
	float     fontSize;
	SInt32    unk04;       // 0004 // Number of font textures. No more than 8 are allowed. See 00574892.
	UInt32    unk08;
	char      name[284];   // 000C // One texture filename (sans path) every 0x24 bytes, e.g. name[0], name[0x24], name[0x48], ...
	GlyphInfo glyphs[256]; // 0128
	//
	// Question: what would multiple texture files accomplish? How would they even work? It seems 
	// like the FontInfo struct only supports one texture file at a time in the first place.
	//
	// If you ensure that there is only one font texture, then you can have the name as long as 
	// you like. TEX names aren't capped to 0x24 bytes; the game just starts reading at multiples 
	// of 0x24 within the name string.
};

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
	struct GlyphInfo
	{
		struct UV {
			union { // x or u: percentage of width
				float x;
				float u;
			};
			union { // y or v: percentage of height
				float y;
				float v;
			};
		};
		//
		float unk00;       // 00
		UV    topLeft;     // 04
		UV    topRight;    // 0C
		UV    bottomLeft;  // 14
		UV    bottomRight; // 1C
		float width;
		float height;
		float kerningLeft;
		float kerningRight;
		float ascent;
		//	totalWidth = width + widthMod
	};

	// 24
	struct TexFileName
	{
		UInt32			textureID;
		char			fileName[0x20];
	};

	// 3928
	struct BufferData
	{
		float			lineHeight;				// 0000
		UInt32			numTextures;			// 0004
		TexFileName		textures[8];			// 0008
		GlyphInfo		glyphs[256];	// 0128
	};

	struct ButtonIcon;

	UInt8						isLoaded;	// 00
	UInt8						pad01[3];	// 01
	char*						filePath;	// 04
	UInt32						fontID;		// 08
	NiTexturingProperty*		fontTexProp;	// 0C
	UInt32						renderState[7];	// 10
	float						maxCharHeight;		// 2C
	float						maxWidthMod;		// 30
	UInt32						unk34;		// 34
	BufferData* fontData;// 38
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

// From JG
//0x11F33F8
// From Modern Minimap
//0x5BD5B0
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

//From Stewie Tweaks
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
		String          text;       // 10
		float           flt18;      // 18    Always -1.0
		NiColorAlpha    color;      // 1C
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
		String text;		// 08
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
			if (!linesPtr->text.m_data) break;
			if (maxY < linesPtr->offsetY)
			{
				maxY = linesPtr->offsetY;
				result = linesPtr;
			}
		} while (--counter);
		return result;
	}

	String* GetDebugInput()
	{
		return &(GetDebugInputLine()->text);
	}
};
STATIC_ASSERT(sizeof(DebugText) == 0x229C);