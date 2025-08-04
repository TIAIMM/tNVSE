#pragma once

#include "Utilities.h"
#include "NiNodes.h"

class NiMultiTargetTransformController;

//From JIP, JGNVSE, Wall & Confused

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

class NiFixedString {
public:
	const char* handle;

	NiFixedString(const char* pcString) {
		if (pcString)
			handle = CdeclCall<const char*>(0xA5B690, pcString);
		else
			handle = nullptr;
	};
	~NiFixedString() {
		CdeclCall(0x4381D0, this);
	};
};

class NiMemObject {
	NiMemObject();
	~NiMemObject();
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

// 08
class NiRefObject : public NiMemObject {
public:
	NiRefObject();
	~NiRefObject();

	virtual void	Destructor(bool freeThis);
	virtual void	Free(void);

	UInt32		m_uiRefCount;	// 04

	inline void IncRefCount() {
		InterlockedIncrement(&m_uiRefCount);
	}

	inline void DecRefCount() {
		if (!InterlockedDecrement(&m_uiRefCount))
			Free();
	}
};

// 08
class NiObject : public NiRefObject {
public:
	NiObject();
	~NiObject();

	virtual NiRTTI* GetType();
	virtual NiNode* GetNiNode();
	virtual BSFadeNode* GetFadeNode();
	virtual void	Unk_05(void);
	virtual void	Unk_06(void);
	virtual void	Unk_07(void);
	virtual void	Unk_08(void);
	virtual void	Unk_09(void);
	virtual void	Unk_0A(void);
	virtual void	Unk_0B(void);
	virtual void	Unk_0C(void);
	virtual void	Unk_0D(void);
	virtual void	Unk_0E(void);
	virtual void	Unk_0F(void);
	virtual void	Unk_10(void);
	virtual void	Unk_11(void);
	virtual void	Unk_12(UInt32 arg);
	virtual void	Unk_13(UInt32 arg);
	virtual void	Unk_14(UInt32 arg);
	virtual void	Unk_15(UInt32 arg);
	virtual void	Unk_16(UInt32 arg);
	virtual void	Unk_17(UInt32 arg);
	virtual void	Unk_18(UInt32 arg);
	virtual void	Unk_19(UInt32 arg);
	virtual void	Unk_1A(UInt32 arg);
	virtual void	Unk_1B(UInt32 arg);
	virtual void	Unk_1C(void);
	virtual void	Unk_1D(void);
	virtual void	Unk_1E(UInt32 arg);
	virtual UInt32	Unk_1F(void);
	virtual void	Unk_20(void);
	virtual void	Unk_21(UInt32 arg);
	virtual void	Unk_22(void);
};

// 18
class NiObjectNET : public NiObject {
public:
	NiObjectNET();
	~NiObjectNET();

	NiFixedString		m_blockName;				// 08
	NiTimeController* m_controller;				// 0C
	NiExtraData** m_extraDataList;			// 10
	UInt16				m_extraDataListLen;			// 14
	UInt16				m_extraDataListCapacity;	// 16
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