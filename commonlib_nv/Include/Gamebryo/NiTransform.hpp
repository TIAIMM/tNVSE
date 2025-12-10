#pragma once

#include "NiMatrix3.hpp"
#include "NiPoint3.hpp"

class NiTransform {
public:
	BS_ALLOCATORS

	NiTransform();

	NiMatrix3	m_Rotate;
	NiPoint3	m_Translate;
	float		m_fScale;

	NiTransform operator*(const NiTransform& xform) const {
		NiTransform out{};
		ThisStdCall<NiTransform*>(0x62C250, this, &out, &xform);
		return out;
	}

	NiPoint3 operator*(const NiPoint3& kPoint) const {
		NiPoint3 out{};
		ThisStdCall<NiPoint3*>(0x524C40, this, &out, &kPoint);
		return out;
	}

	bool operator==(const NiTransform& kTransform) const {
		return m_Rotate == kTransform.m_Rotate && m_Translate == kTransform.m_Translate && m_fScale == kTransform.m_fScale;
	}

	void MakeIdentity();
	void Invert(NiTransform& kDest);

	NiTransform GetInverse() {
		NiTransform out{};
		ThisStdCall(0x4B4880, this, &out);
		return out;
	}
};

ASSERT_SIZE(NiTransform, 0x34);