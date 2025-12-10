#pragma once

#include "NiTMap.hpp"
#include "NiFixedString.hpp"

template <class T_Data>
class NiTFixedStringMapBase : public NiMemObject {
public:
	NiTFixedStringMapBase(UInt32 uiHashSize = 0x100);
	virtual ~NiTFixedStringMapBase();
	virtual NiTMapItem<NiFixedString, T_Data>* NewItem();
	virtual void DeleteItem(NiTMapItem<NiFixedString, T_Data>* apItem);

	UInt32								m_uiHashSize;
	NiTMapItem<NiFixedString, T_Data>** m_ppkHashTable;
	UInt32								m_uiCount;
};

template <class T_Data>
class NiTFixedStringMap : public NiTFixedStringMapBase<T_Data> {
public:
};

ASSERT_SIZE(NiTFixedStringMap<UInt32>, 0x10);