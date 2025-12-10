#pragma once

#include "NiTriBasedGeomData.hpp"

NiSmartPointer(NiTriStripsData);

class NiTriStripsData : public NiTriBasedGeomData {
public:
	NiTriStripsData();
	virtual ~NiTriStripsData();

	UInt16	m_usStrips;
	UInt16* m_pusStripLengths;
	UInt16* m_pusStripLists;

	CREATE_OBJECT(NiTriStripsData, 0xA75EC0);
	NIRTTI_ADDRESS(0x11F4A70);

	static NiTriStripsData* Create(UInt16 ausVertices, NiPoint3* apVertex, NiPoint3* apNormal, NiColorA* apColor, NiPoint2* apTexture, UInt16 ausNumTextureSets, DataFlags aeNBTMethod, UInt16 ausTriangles, UInt16 ausStrips, UInt16* apusStripLengths, UInt16* apusStripLists);

	__forceinline UInt16 GetStripCount() const {
		return m_usStrips;
	}

	__forceinline UInt16* GetStripLengths() const {
		return m_pusStripLengths;
	}

	__forceinline UInt16* GetStripLists() const {
		return m_pusStripLists;
	}

	__forceinline UInt16* GetIndices() const {
		return m_pusStripLists;
	}

	__forceinline UInt32 GetIndexCount() const {
		return m_pusStripLengths[0];
	}

	__forceinline UInt32 GetStripLengthSum() const {
		return m_usTriangles + 2 * m_usStrips;
	}

	void Replace(UInt16 usStrips, UInt16* apusStripLengths, UInt16* apusStripLists);
};

ASSERT_SIZE(NiTriStripsData, 0x50);