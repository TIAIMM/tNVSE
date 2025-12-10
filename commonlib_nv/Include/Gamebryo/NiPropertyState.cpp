#include "NiPropertyState.hpp"

// 0xA89AB0
NiPropertyState::NiPropertyState() {
	for (UInt32 i = 0; i < PropertyID::MAX; i++) {
		m_aspProps[i].m_pObject = nullptr;
	}
	Reset();
}

// 0xA89810
NiPropertyState::NiPropertyState(const NiPropertyState& arSource) {
	for (UInt32 i = 0; i < PropertyID::MAX; i++) {
		m_aspProps[i] = arSource.m_aspProps[i];
	}
}

// 0xA89890
NiPropertyState::~NiPropertyState() {
	for (UInt32 i = 0; i < PropertyID::MAX; i++) {
		m_aspProps[i] = nullptr;
	}
}

// 0xA89960
void NiPropertyState::Reset() {
	ThisStdCall(0xA89960, this);
}
