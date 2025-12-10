#pragma once

#include "NiMemObject.hpp"
#include "NiTMallocInterface.hpp"
#include "NiTNewInterface.hpp"

template <class T_Data, class Allocator>
class NiTSet : public NiMemObject {
public:
	NiTSet(UInt32 auiInitialSize) {
		if (auiInitialSize > 0) {
			m_pBase = Allocator::Allocate(auiInitialSize);
		}
		else {
			m_pBase = nullptr;
		}
		m_uiAlloced = auiInitialSize;
		m_uiUsed = 0;
	}
	~NiTSet() {
		Allocator::Deallocate(m_pBase);
	}

	NiTSet(const NiTSet&) = delete;
	NiTSet& operator=(const NiTSet&) = delete;

	T_Data* m_pBase;
	UInt32	m_uiAlloced;
	UInt32	m_uiUsed;

	UInt32 GetSize() const { return m_uiUsed; }
	T_Data* GetBase() const { return m_pBase; }
	const T_Data& GetAt(UInt32 auiIndex) const { return m_pBase[auiIndex]; }
	T_Data& GetAt(UInt32 auiIndex) { return m_pBase[auiIndex]; }

	void Realloc(unsigned int uiNewSize) {
		if (uiNewSize != m_uiAlloced) {
			T_Data* pNewBase;
			unsigned int i;

			if (uiNewSize > 0) {
				pNewBase = Allocator::Allocate(uiNewSize);

				for (i = 0; i < m_uiUsed; i++) {
					pNewBase[i] = m_pBase[i];
				}
			}
			else {
				pNewBase = NULL;
			}

			Allocator::Deallocate(m_pBase);
			m_pBase = pNewBase;
			m_uiAlloced = uiNewSize;
		}
	}

	void Add(const T_Data& element) {
		if (m_uiUsed == m_uiAlloced) {
			Realloc(m_uiAlloced > 0 ? (2 * m_uiAlloced) : 1);
		}

		m_pBase[m_uiUsed++] = element;
	}


	struct Iterator
	{
		friend NiTSet;

		UInt32 index;
		NiTSet* set;

		T_Data& operator*() const { return set->m_pBase[index]; }

		Iterator& operator++() {
			index++;
			return *this;
		}

		bool operator!=(const Iterator& other) {
			return index != other.index;
		}
	};

	Iterator begin() {
		return Iterator{ 0, this };
	}

	Iterator end() {
		return Iterator{ m_uiUsed, this };
	}
};

template <class T_Data> class NiTObjectSet : public NiTSet<T_Data, NiTNewInterface<T_Data>> {
public:
	NiTObjectSet(UInt32 auiInitialSize = 0) : NiTSet<T_Data, NiTNewInterface<T_Data>>(auiInitialSize) {};
};

template <class T_Data> class NiTPrimitiveSet : public NiTSet<T_Data, NiTMallocInterface<T_Data>> {
public:
	NiTPrimitiveSet(UInt32 auiInitialSize = 0) : NiTSet<T_Data, NiTMallocInterface<T_Data>>(auiInitialSize) {};
};

typedef NiTPrimitiveSet<UInt32> NiUnsignedIntSet;
typedef NiTPrimitiveSet<UInt16> NiUnsignedShortSet;
typedef NiTPrimitiveSet<float> NiFloatSet;