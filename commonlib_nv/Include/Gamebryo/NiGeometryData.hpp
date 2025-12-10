#pragma once

#include "NiObject.hpp"
#include "NiBound.hpp"
#include "NiColor.hpp"
#include "NiPoint2.hpp"
#include "NiAdditionalGeometryData.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiShaderDeclaration.hpp"

NiSmartPointer(NiGeometryData);

class NiTriStripsData;
class NiTriShapeData;
class BSAdditionalGeomDataBlock;

class NiGeometryData : public NiObject {
public:
	NiGeometryData();
	virtual ~NiGeometryData();

	virtual void				SetActiveVertexCount(UInt16 usActive);
	virtual UInt16				GetActiveVertexCount();
	virtual NiTriStripsData*	IsStripsData();
	virtual NiTriShapeData*		IsShapeData();
	virtual bool				ContainsVertexData(NiShaderDeclaration::ShaderParameter eParameter);
	virtual void				CalculateNormals();

	enum Consistency {
		MUTABLE				= 0x0000,
		STATIC				= 0x4000,
		CONSISTENCY_MASK	= 0x7000,
	};

	enum KeepFlags {
		KEEP_NONE		= 0,
		KEEP_XYZ		= 1 << 0,
		KEEP_NORM		= 1 << 1,
		KEEP_COLOR		= 1 << 2,
		KEEP_UV			= 1 << 3,
		KEEP_INDICES	= 1 << 4,
		KEEP_BONEDATA	= 1 << 5,
		KEEP_ALL		= 0x3F,
	};

	enum DataFlags {
		NBT_METHOD_NONE			= 0x0000,
		NBT_METHOD_NDL			= 0x1000,
		NBT_METHOD_DEPRECATED	= 0x2000,
		NBT_METHOD_ATI			= 0x3000,
		NBT_METHOD_MASK			= 0xF000,
		TEXTURE_SET_MASK		= 0x3F
	};

	enum Compression {
		COMPRESS_NORM		= 1 << 0,
		COMPRESS_COLOR		= 1 << 1,
		COMPRESS_UV			= 1 << 2,
		COMPRESS_WEIGHT		= 1 << 3,
		COMPRESS_POSITION	= 1 << 4,
		COMPRESS_ALL		= 0x1F,
	};

	enum MarkAsChangedFlags {
		VERTEX_MASK		= 1 << 0,
		NORMAL_MASK		= 1 << 1,
		COLOR_MASK		= 1 << 2,
		TEXTURE_MASK	= 1 << 3,
		ALL_MASK		= VERTEX_MASK | NORMAL_MASK | COLOR_MASK | TEXTURE_MASK,
		DIRTY_MASK		= 0xFFF,
	};

	UInt16						m_usVertices;
	UInt16						m_usID;
	UInt16						m_usDataFlags;
	UInt16						m_usDirtyFlags;
	NiBound						m_kBound;
	NiPoint3*					m_pkVertex;
	NiPoint3*					m_pkNormal;
	NiColorA*					m_pkColor;
	NiPoint2*					m_pkTexture;
	NiAdditionalGeometryDataPtr m_spAdditionalGeomData;
	NiGeometryBufferData*		m_pkBuffData;
	UInt8						m_ucKeepFlags;
	UInt8						m_ucCompressFlags;
	bool						bIsReadingData;
	bool						bUnk3B;
	bool						bCanSave;

	static NiGeometryData* Create(UInt16 usVertices, NiPoint3* pkVertex, NiPoint3* pkNormal, NiColorA* pkColor, NiPoint2* pkTexture, UInt16 usNumTextureSets, UInt32 eNBTMethod);
	static NiGeometryData* Create();

	static NiGeometryData* __fastcall CreateEx(NiGeometryData* apThis, void*, UInt16 usVertices, NiPoint3* pkVertex, NiPoint3* pkNormal, NiColorA* pkColor, NiPoint2* pkTexture, UInt16 usNumTextureSets, UInt32 eNBTMethod);


	void LoadBinaryEx(NiStream* kStream);
	void SaveBinaryEx(NiStream* kStream);

	NiPoint3* GetVertices() const;
	NiPoint3* GetNormals() const;
	NiPoint3* GetBinormals() const;
	NiPoint3* GetTangents() const;
	NiColorA* GetColors() const;
	NiPoint2* GetTextures() const;

	UInt16 GetTextureSets() const;
	NiPoint2* GetTextureSet(UInt16 ausSet);

	UInt16 GetVertexCount() const;

	NiGeometryData::DataFlags GetNormalBinormalTangentMethod() const;

	void SetKeepFlags(KeepFlags aeFlags);
	void SetCompressFlags(Compression aeFlags);

	NiGeometryData::Consistency GetConsistency() const;
	void SetConsistency(Consistency aeFlags);

	bool StartReadingAdditionalData(UInt32 unk = 1);
	void StopReadingAdditionalData();

	void SetAdditionalGeomData(NiAdditionalGeometryData* apData);
	void GetAdditionalVertexData(BSAdditionalGeomDataBlock* apData) const;
	void GetAdditionalNormalData(BSAdditionalGeomDataBlock* apData) const;
	void GetAdditionalColorData(BSAdditionalGeomDataBlock* apData) const;
	void GetAdditionalTextureData(BSAdditionalGeomDataBlock* apData) const;

	void MarkAsChanged(UInt32 aeFlags);
	void ClearRevisionID();

	void Replace(UInt16 ausVertices, NiPoint3* apVertex, NiPoint3* apNormal, NiColorA* apColor, NiPoint2* apTexture, UInt16 ausNumTextureSets, NiGeometryData::DataFlags aeNBTMethod);
	void ReplaceData(UInt16 ausVertices, NiPoint3* apVertex, NiPoint3* apNormal, NiColorA* apColor, NiPoint2* apTexture, UInt16 ausNumTextureSets, NiGeometryData::DataFlags aeNBTMethod);
};

ASSERT_SIZE(NiGeometryData, 0x40);