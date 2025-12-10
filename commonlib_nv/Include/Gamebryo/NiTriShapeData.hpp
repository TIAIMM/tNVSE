#pragma once

#include "NiTriBasedGeomData.hpp"

NiSmartPointer(NiTriShapeData);

class NiTriShapeData : public NiTriBasedGeomData {
public:
	NiTriShapeData();
	virtual ~NiTriShapeData();

	class SharedNormalArray : public NiMemObject {
	public:
		UInt16	m_usNumSharedNormals;
		UInt16* m_pusSharedNormalIndexArray;
	};

	class SNAMemBlock : public NiMemObject {
	public:
		UInt16* m_pusBlock;
		UInt16* m_pusFreeBlock;
		UInt32	m_uiBlockSize;
		UInt32	m_uiFreeBlockSize;

		SNAMemBlock* m_pkNext;
	};

	UInt32				m_uiTriListLength;
	UInt16*				m_pusTriList;
	SharedNormalArray*	m_pkSharedNormals;
	UInt16				m_usSharedNormalsArraySize;
	SNAMemBlock*		m_pkSNAMemoryBlocks;

	CREATE_OBJECT(NiTriShapeData, 0xA7B790);
	NIRTTI_ADDRESS(0x11F4A88);

	static NiTriShapeData* Create(UInt16 usVertices, NiPoint3* pkVertex, NiPoint3* pkNormal, NiColorA* pkColor, NiPoint2* pkTexture, UInt16 usNumTextureSets, UInt32 eNBTMethod, UInt16 usTriangles, UInt16* pusTriList);

	UInt32 GetTriListLength() const;
	UInt16* GetTriList() const;

	void SetTriangleIndices(UInt16 i, UInt16 i0, UInt16 i1, UInt16 i2) {
		m_pusTriList[3 * i] = i0;
		m_pusTriList[3 * i + 1] = i1;
		m_pusTriList[3 * i + 2] = i2;
	}


	__forceinline UInt32 GetIndexCount() const {
		return GetTriListLength();
	}
};

ASSERT_SIZE(NiTriShapeData, 0x58)