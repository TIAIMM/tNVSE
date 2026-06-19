#pragma once

#include "NiMatrix3.hpp"

// 0x11A9448
const NiMatrix3 NiMatrix3::IDENTITY = NiMatrix3(1.0f, 0.0f, 0.0f,
												0.0f, 1.0f, 0.0f,
												0.0f, 0.0f, 1.0f);


bool NiMatrix3::operator==(const NiMatrix3& mat) const {
	return
		(m_pEntry[0][0] == mat.m_pEntry[0][0]) &&
		(m_pEntry[0][1] == mat.m_pEntry[0][1]) &&
		(m_pEntry[0][2] == mat.m_pEntry[0][2]) &&
		(m_pEntry[1][0] == mat.m_pEntry[1][0]) &&
		(m_pEntry[1][1] == mat.m_pEntry[1][1]) &&
		(m_pEntry[1][2] == mat.m_pEntry[1][2]) &&
		(m_pEntry[2][0] == mat.m_pEntry[2][0]) &&
		(m_pEntry[2][1] == mat.m_pEntry[2][1]) &&
		(m_pEntry[2][2] == mat.m_pEntry[2][2]);
}

NiMatrix3 NiMatrix3::operator+ (const NiMatrix3& mat) const {
	NiMatrix3 result = *this;
	result.m_pEntry[0][0] += mat.m_pEntry[0][0];
	result.m_pEntry[0][1] += mat.m_pEntry[0][1];
	result.m_pEntry[0][2] += mat.m_pEntry[0][2];
	result.m_pEntry[1][0] += mat.m_pEntry[1][0];
	result.m_pEntry[1][1] += mat.m_pEntry[1][1];
	result.m_pEntry[1][2] += mat.m_pEntry[1][2];
	result.m_pEntry[2][0] += mat.m_pEntry[2][0];
	result.m_pEntry[2][1] += mat.m_pEntry[2][1];
	result.m_pEntry[2][2] += mat.m_pEntry[2][2];
	return result;
}

NiMatrix3 NiMatrix3::operator- (const NiMatrix3& mat) const {
	NiMatrix3 result = *this;
	result.m_pEntry[0][0] -= mat.m_pEntry[0][0];
	result.m_pEntry[0][1] -= mat.m_pEntry[0][1];
	result.m_pEntry[0][2] -= mat.m_pEntry[0][2];
	result.m_pEntry[1][0] -= mat.m_pEntry[1][0];
	result.m_pEntry[1][1] -= mat.m_pEntry[1][1];
	result.m_pEntry[1][2] -= mat.m_pEntry[1][2];
	result.m_pEntry[2][0] -= mat.m_pEntry[2][0];
	result.m_pEntry[2][1] -= mat.m_pEntry[2][1];
	result.m_pEntry[2][2] -= mat.m_pEntry[2][2];
	return result;
}

NiMatrix3 NiMatrix3::operator* (const NiMatrix3& mat) const {
	NiMatrix3 result;
	result.m_pEntry[0][0] =
		m_pEntry[0][0] * mat.m_pEntry[0][0] +
		m_pEntry[0][1] * mat.m_pEntry[1][0] +
		m_pEntry[0][2] * mat.m_pEntry[2][0];
	result.m_pEntry[1][0] =
		m_pEntry[1][0] * mat.m_pEntry[0][0] +
		m_pEntry[1][1] * mat.m_pEntry[1][0] +
		m_pEntry[1][2] * mat.m_pEntry[2][0];
	result.m_pEntry[2][0] =
		m_pEntry[2][0] * mat.m_pEntry[0][0] +
		m_pEntry[2][1] * mat.m_pEntry[1][0] +
		m_pEntry[2][2] * mat.m_pEntry[2][0];
	result.m_pEntry[0][1] =
		m_pEntry[0][0] * mat.m_pEntry[0][1] +
		m_pEntry[0][1] * mat.m_pEntry[1][1] +
		m_pEntry[0][2] * mat.m_pEntry[2][1];
	result.m_pEntry[1][1] =
		m_pEntry[1][0] * mat.m_pEntry[0][1] +
		m_pEntry[1][1] * mat.m_pEntry[1][1] +
		m_pEntry[1][2] * mat.m_pEntry[2][1];
	result.m_pEntry[2][1] =
		m_pEntry[2][0] * mat.m_pEntry[0][1] +
		m_pEntry[2][1] * mat.m_pEntry[1][1] +
		m_pEntry[2][2] * mat.m_pEntry[2][1];
	result.m_pEntry[0][2] =
		m_pEntry[0][0] * mat.m_pEntry[0][2] +
		m_pEntry[0][1] * mat.m_pEntry[1][2] +
		m_pEntry[0][2] * mat.m_pEntry[2][2];
	result.m_pEntry[1][2] =
		m_pEntry[1][0] * mat.m_pEntry[0][2] +
		m_pEntry[1][1] * mat.m_pEntry[1][2] +
		m_pEntry[1][2] * mat.m_pEntry[2][2];
	result.m_pEntry[2][2] =
		m_pEntry[2][0] * mat.m_pEntry[0][2] +
		m_pEntry[2][1] * mat.m_pEntry[1][2] +
		m_pEntry[2][2] * mat.m_pEntry[2][2];
	return result;
}

NiMatrix3 NiMatrix3::operator* (float fScalar) const {
	NiMatrix3 result = *this;
	result.m_pEntry[0][0] *= fScalar;
	result.m_pEntry[0][1] *= fScalar;
	result.m_pEntry[0][2] *= fScalar;
	result.m_pEntry[1][0] *= fScalar;
	result.m_pEntry[1][1] *= fScalar;
	result.m_pEntry[1][2] *= fScalar;
	result.m_pEntry[2][0] *= fScalar;
	result.m_pEntry[2][1] *= fScalar;
	result.m_pEntry[2][2] *= fScalar;
	return result;
}

NiPoint3 NiMatrix3::operator*(const NiPoint3& pt) const {
	return NiPoint3(
		m_pEntry[0][0] * pt.x + m_pEntry[0][1] * pt.y + m_pEntry[0][2] * pt.z,
		m_pEntry[1][0] * pt.x + m_pEntry[1][1] * pt.y + m_pEntry[1][2] * pt.z,
		m_pEntry[2][0] * pt.x + m_pEntry[2][1] * pt.y + m_pEntry[2][2] * pt.z);
}

NiPoint3 operator*(const NiPoint3& pt, const NiMatrix3& mat) {
	return NiPoint3(
		mat.m_pEntry[0][0] * pt.x + mat.m_pEntry[1][0] * pt.y + mat.m_pEntry[2][0] * pt.z,
		mat.m_pEntry[0][1] * pt.x + mat.m_pEntry[1][1] * pt.y + mat.m_pEntry[2][1] * pt.z,
		mat.m_pEntry[0][2] * pt.x + mat.m_pEntry[1][2] * pt.y + mat.m_pEntry[2][2] * pt.z);
}

NiMatrix3 NiMatrix3::operator/ (float fScalar) const {
	NiMatrix3 result = *this;
	result.m_pEntry[0][0] /= fScalar;
	result.m_pEntry[0][1] /= fScalar;
	result.m_pEntry[0][2] /= fScalar;
	result.m_pEntry[1][0] /= fScalar;
	result.m_pEntry[1][1] /= fScalar;
	result.m_pEntry[1][2] /= fScalar;
	result.m_pEntry[2][0] /= fScalar;
	result.m_pEntry[2][1] /= fScalar;
	result.m_pEntry[2][2] /= fScalar;
	return result;
}

void NiMatrix3::MakeXRotation(float fAngle) {
	float sn = std::sin(fAngle);
	float cs = std::cos(fAngle);

	m_pEntry[0][0] = 1.0f;
	m_pEntry[0][1] = 0.0f;
	m_pEntry[0][2] = 0.0f;
	m_pEntry[1][0] = 0.0f;
	m_pEntry[1][1] = cs;
	m_pEntry[1][2] = sn;
	m_pEntry[2][0] = 0.0f;
	m_pEntry[2][1] = -sn;
	m_pEntry[2][2] = cs;
}

void NiMatrix3::MakeYRotation(float fAngle) {
	float sn = std::sin(fAngle);
	float cs = std::cos(fAngle);

	m_pEntry[0][0] = cs;
	m_pEntry[0][1] = 0.0f;
	m_pEntry[0][2] = -sn;
	m_pEntry[1][0] = 0.0f;
	m_pEntry[1][1] = 1.0f;
	m_pEntry[1][2] = 0.0f;
	m_pEntry[2][0] = sn;
	m_pEntry[2][1] = 0.0f;
	m_pEntry[2][2] = cs;
}

void NiMatrix3::MakeZRotation(float fAngle) {
	float sn = std::sin(fAngle);
	float cs = std::cos(fAngle);

	m_pEntry[0][0] = cs;
	m_pEntry[0][1] = sn;
	m_pEntry[0][2] = 0.0f;
	m_pEntry[1][0] = -sn;
	m_pEntry[1][1] = cs;
	m_pEntry[1][2] = 0.0f;
	m_pEntry[2][0] = 0.0f;
	m_pEntry[2][1] = 0.0f;
	m_pEntry[2][2] = 1.0f;
}

void NiMatrix3::MakeRotation(float fAngle, float x, float y, float z) {
	float sn = std::sin(fAngle);
	float cs = std::cos(fAngle);

	float omcs = 1.0f - cs;
	float x2 = x * x;
	float y2 = y * y;
	float z2 = z * z;
	float xym = x * y * omcs;
	float xzm = x * z * omcs;
	float yzm = y * z * omcs;
	float xsin = x * sn;
	float ysin = y * sn;
	float zsin = z * sn;

	m_pEntry[0][0] = x2 * omcs + cs;
	m_pEntry[0][1] = xym + zsin;
	m_pEntry[0][2] = xzm - ysin;
	m_pEntry[1][0] = xym - zsin;
	m_pEntry[1][1] = y2 * omcs + cs;
	m_pEntry[1][2] = yzm + xsin;
	m_pEntry[2][0] = xzm + ysin;
	m_pEntry[2][1] = yzm - xsin;
	m_pEntry[2][2] = z2 * omcs + cs;
}

void NiMatrix3::MakeRotation(float angle, const NiPoint3& axis) {
	MakeRotation(angle, axis.x, axis.y, axis.z);
}

void NiMatrix3::FromEulerAnglesPRY(float pitch, float roll, float yaw) {
	NiMatrix3 mPitch, mRoll, mYaw;
	mPitch.MakeXRotation(pitch);
	mRoll.MakeYRotation(roll);
	mYaw.MakeZRotation(yaw);
	*this = mPitch * mRoll * mYaw;
}

void NiMatrix3::FromEulerAnglesYPR(const float pitch, const float roll, const float yaw) {
	NiMatrix3 mPitch, mRoll, mYaw;
	mPitch.MakeXRotation(pitch);
	mRoll.MakeYRotation(roll);
	mYaw.MakeZRotation(yaw);
	*this = mYaw * mPitch * mRoll;
}

const float NI_INFINITY = FLT_MAX;
const float NI_PI = 3.1415927410125732f;
const float NI_HALF_PI = 0.5f * NI_PI;
const float NI_TWO_PI = 2.0f * NI_PI;

bool NiMatrix3::ToEulerAnglesXYZ(float& arfXAngle, float& arfYAngle, float& arfZAngle) const {
	arfYAngle = -asin(m_pEntry[0][2]);
	if (arfYAngle < NI_HALF_PI) {
		if (arfYAngle > -NI_HALF_PI) {
			arfXAngle = -atan2(-m_pEntry[1][2], m_pEntry[2][2]);
			arfZAngle = -atan2(-m_pEntry[0][1], m_pEntry[0][0]);
			return true;
		}
		else {
			// WARNING.  Not a unique solution.
			float fRmY = atan2(m_pEntry[1][0], m_pEntry[1][1]);
			arfZAngle = 0.0f;  // any angle works
			arfXAngle = fRmY - arfZAngle;
			return false;
		}
	}
	else {
		// WARNING.  Not a unique solution.
		float fRpY = atan2(m_pEntry[1][0], m_pEntry[1][1]);
		arfZAngle = 0.0f;  // any angle works
		arfXAngle = arfZAngle - fRpY;
		return false;
	}
}

NiMatrix3 NiMatrix3::TransposeTimes(const NiMatrix3& mat) const {
	NiMatrix3 prd;
	prd.m_pEntry[0][0] =
		m_pEntry[0][0] * mat.m_pEntry[0][0] +
		m_pEntry[1][0] * mat.m_pEntry[1][0] +
		m_pEntry[2][0] * mat.m_pEntry[2][0];
	prd.m_pEntry[1][0] =
		m_pEntry[0][1] * mat.m_pEntry[0][0] +
		m_pEntry[1][1] * mat.m_pEntry[1][0] +
		m_pEntry[2][1] * mat.m_pEntry[2][0];
	prd.m_pEntry[2][0] =
		m_pEntry[0][2] * mat.m_pEntry[0][0] +
		m_pEntry[1][2] * mat.m_pEntry[1][0] +
		m_pEntry[2][2] * mat.m_pEntry[2][0];
	prd.m_pEntry[0][1] =
		m_pEntry[0][0] * mat.m_pEntry[0][1] +
		m_pEntry[1][0] * mat.m_pEntry[1][1] +
		m_pEntry[2][0] * mat.m_pEntry[2][1];
	prd.m_pEntry[1][1] =
		m_pEntry[0][1] * mat.m_pEntry[0][1] +
		m_pEntry[1][1] * mat.m_pEntry[1][1] +
		m_pEntry[2][1] * mat.m_pEntry[2][1];
	prd.m_pEntry[2][1] =
		m_pEntry[0][2] * mat.m_pEntry[0][1] +
		m_pEntry[1][2] * mat.m_pEntry[1][1] +
		m_pEntry[2][2] * mat.m_pEntry[2][1];
	prd.m_pEntry[0][2] =
		m_pEntry[0][0] * mat.m_pEntry[0][2] +
		m_pEntry[1][0] * mat.m_pEntry[1][2] +
		m_pEntry[2][0] * mat.m_pEntry[2][2];
	prd.m_pEntry[1][2] =
		m_pEntry[0][1] * mat.m_pEntry[0][2] +
		m_pEntry[1][1] * mat.m_pEntry[1][2] +
		m_pEntry[2][1] * mat.m_pEntry[2][2];
	prd.m_pEntry[2][2] =
		m_pEntry[0][2] * mat.m_pEntry[0][2] +
		m_pEntry[1][2] * mat.m_pEntry[1][2] +
		m_pEntry[2][2] * mat.m_pEntry[2][2];
	return prd;
}

bool NiMatrix3::Inverse(NiMatrix3& inv) const {
	inv.m_pEntry[0][0] = m_pEntry[1][1] * m_pEntry[2][2] - m_pEntry[1][2] * m_pEntry[2][1];
	inv.m_pEntry[0][1] = m_pEntry[0][2] * m_pEntry[2][1] - m_pEntry[0][1] * m_pEntry[2][2];
	inv.m_pEntry[0][2] = m_pEntry[0][1] * m_pEntry[1][2] - m_pEntry[0][2] * m_pEntry[1][1];
	inv.m_pEntry[1][0] = m_pEntry[1][2] * m_pEntry[2][0] - m_pEntry[1][0] * m_pEntry[2][2];
	inv.m_pEntry[1][1] = m_pEntry[0][0] * m_pEntry[2][2] - m_pEntry[0][2] * m_pEntry[2][0];
	inv.m_pEntry[1][2] = m_pEntry[0][2] * m_pEntry[1][0] - m_pEntry[0][0] * m_pEntry[1][2];
	inv.m_pEntry[2][0] = m_pEntry[1][0] * m_pEntry[2][1] - m_pEntry[1][1] * m_pEntry[2][0];
	inv.m_pEntry[2][1] = m_pEntry[0][1] * m_pEntry[2][0] - m_pEntry[0][0] * m_pEntry[2][1];
	inv.m_pEntry[2][2] = m_pEntry[0][0] * m_pEntry[1][1] - m_pEntry[0][1] * m_pEntry[1][0];

	const float fDet = m_pEntry[0][0] * inv.m_pEntry[0][0] +
				 m_pEntry[0][1] * inv.m_pEntry[1][0] +
				 m_pEntry[0][2] * inv.m_pEntry[2][0];

	if (std::abs(fDet) <= 1e-06f)
		return false;

	const float fInvdet = 1.0f / fDet;
	for (auto& row : inv.m_pEntry) {
		for (float& col : row) {
			col *= fInvdet;
		}
	}

	return true;
}

D3DMATRIX NiMatrix3::ToD3D() const {
	D3DMATRIX result;
	result._11 = GetEntry(0, 0);
	result._12 = GetEntry(1, 0);
	result._13 = GetEntry(2, 0);
	result._14 = 0.0f;
	result._21 = GetEntry(0, 1);
	result._22 = GetEntry(1, 1);
	result._23 = GetEntry(2, 1);
	result._24 = 0.0f;
	result._31 = GetEntry(0, 2);
	result._32 = GetEntry(1, 2);
	result._33 = GetEntry(2, 2);
	result._34 = 0.0f;
	result._41 = 0.0f;
	result._42 = 0.0f;
	result._43 = 0.0f;
	result._44 = 1.0f;
	return result;
}

// 0xA598F0
const char* NiMatrix3::GetViewerStrings(const char* apText) {
	return ThisStdCall<const char*>(0xA598F0, this, apText);
}
