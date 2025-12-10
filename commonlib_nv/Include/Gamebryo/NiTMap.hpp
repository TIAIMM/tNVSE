#pragma once

#include "NiTMapBase.hpp"
#include "NiTDefaultAllocator.hpp"

template <class T_Key, class T_Data>
class NiTMap : public NiTMapBase<NiTDefaultAllocator<T_Data>, T_Key, T_Data> {
public:
	NiTMap(UInt32 uiHashSize = 37) : NiTMapBase<NiTDefaultAllocator<T_Data>, T_Key, T_Data>(uiHashSize) {};
	~NiTMap() override { NiTMapBase<NiTDefaultAllocator<T_Data>, T_Key, T_Data>::RemoveAll(); };

	NiTMapItem<T_Key, T_Data>* NewItem() override;
	void DeleteItem(NiTMapItem<T_Key, T_Data>* pkItem) override;
};

template<class T_Key, class T_Data>
inline NiTMapItem<T_Key, T_Data>* NiTMap<T_Key, T_Data>::NewItem() {
	return reinterpret_cast<NiTMapItem<T_Key, T_Data>*>(NiTMapBase<NiTDefaultAllocator<T_Data>, T_Key, T_Data>::m_kAllocator.
		Allocate());
}

template<class T_Key, class T_Data>
inline void NiTMap<T_Key, T_Data>::DeleteItem(NiTMapItem<T_Key, T_Data>* pkItem) {
	pkItem->m_val = 0;
	NiTMapBase<NiTDefaultAllocator<T_Data>, T_Key, T_Data>::m_kAllocator.Deallocate(pkItem);
}


static_assert(sizeof(NiTMap<UInt32, UInt32>) == 0x10, "Wrong structure size!");