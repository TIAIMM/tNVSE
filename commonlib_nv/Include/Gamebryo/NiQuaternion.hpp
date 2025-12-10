#pragma once
#include "NiMatrix3.hpp"
#include "NiMemObject.hpp"
#include "NiStream.hpp"

class NiMatrix3;
class NiQuaternion : public NiMemObject {
public:
	float m_fW;
	float m_fX;
	float m_fY;
	float m_fZ;

	NiQuaternion() : m_fW(0), m_fX(0), m_fY(0), m_fZ(0) {};
	NiQuaternion(float w, float x, float y, float z) : m_fW(w), m_fX(x), m_fY(y), m_fZ(z)  {};

	NiQuaternion operator+(const NiQuaternion& rhs) const {
		return {m_fW + rhs.m_fW, m_fX + rhs.m_fX, m_fY + rhs.m_fY, m_fZ + rhs.m_fZ};
	}

	NiQuaternion operator-(const NiQuaternion& rhs) const {
		return {m_fW - rhs.m_fW, m_fX - rhs.m_fX, m_fY - rhs.m_fY, m_fZ - rhs.m_fZ};
	}

	NiQuaternion operator*(const float& rhs) const {
		return {m_fW * rhs, m_fX * rhs, m_fY * rhs, m_fZ * rhs};
	}

	NiQuaternion operator/(const float& rhs) const {
		return {m_fW / rhs, m_fX / rhs, m_fY / rhs, m_fZ / rhs};
	}

	NiQuaternion& operator+=(const NiQuaternion& rhs) {
		m_fW += rhs.m_fW;
		m_fX += rhs.m_fX;
		m_fY += rhs.m_fY;
		m_fZ += rhs.m_fZ;
		return *this;
	}

	NiQuaternion& operator-=(const NiQuaternion& rhs) {
		m_fW -= rhs.m_fW;
		m_fX -= rhs.m_fX;
		m_fY -= rhs.m_fY;
		m_fZ -= rhs.m_fZ;
		return *this;
	}

	NiQuaternion& operator*=(const float& rhs) {
		m_fW *= rhs;
		m_fX *= rhs;
		m_fY *= rhs;
		m_fZ *= rhs;
		return *this;
	}

	NiQuaternion& operator*=(const NiQuaternion& q2) {
		// Quaternion multiplication follows the formula:
	    // w = w1*w2 - x1*x2 - y1*y2 - z1*z2
	    // x = w1*x2 + x1*w2 + y1*z2 - z1*y2
	    // y = w1*y2 - x1*z2 + y1*w2 + z1*x2
	    // z = w1*z2 + x1*y2 - y1*x2 + z1*w2

	    const float w = (m_fW * q2.m_fW) - (m_fX * q2.m_fX) - 
						(m_fY * q2.m_fY) - (m_fZ * q2.m_fZ);
	              
		const float x = (m_fW * q2.m_fX) + (m_fX * q2.m_fW) +
						(m_fY * q2.m_fZ) - (m_fZ * q2.m_fY);
	              
		const float y = (m_fW * q2.m_fY) - (m_fX * q2.m_fZ) +
						(m_fY * q2.m_fW) + (m_fZ * q2.m_fX);
	              
		const float z = (m_fW * q2.m_fZ) + (m_fX * q2.m_fY) -
						(m_fY * q2.m_fX) + (m_fZ * q2.m_fW);

	    m_fW = w;
	    m_fX = x;
	    m_fY = y;
	    m_fZ = z;
		return *this;
	}

	NiQuaternion operator*(const NiQuaternion& q2) const {
		// Quaternion multiplication follows the formula:
		// w = w1*w2 - x1*x2 - y1*y2 - z1*z2
		// x = w1*x2 + x1*w2 + y1*z2 - z1*y2
		// y = w1*y2 - x1*z2 + y1*w2 + z1*x2
		// z = w1*z2 + x1*y2 - y1*x2 + z1*w2

		const float w = (m_fW * q2.m_fW) - (m_fX * q2.m_fX) -
			(m_fY * q2.m_fY) - (m_fZ * q2.m_fZ);

		const float x = (m_fW * q2.m_fX) + (m_fX * q2.m_fW) +
			(m_fY * q2.m_fZ) - (m_fZ * q2.m_fY);

		const float y = (m_fW * q2.m_fY) - (m_fX * q2.m_fZ) +
			(m_fY * q2.m_fW) + (m_fZ * q2.m_fX);

		const float z = (m_fW * q2.m_fZ) + (m_fX * q2.m_fY) -
			(m_fY * q2.m_fX) + (m_fZ * q2.m_fW);

		return {w, x, y, z};
	}

	void FastNormalize() {
		ThisStdCall(0xA4FF00, this);
	}

	static NiQuaternion FromRotation(const NiMatrix3* rot) {
		const NiQuaternion result{};
		ThisStdCall(0xA6DF40, &result, rot);
		return result;
	}

	static NiQuaternion FromEulerAngles(const float pitch, const float roll, const float yaw) {
		NiQuaternion result{};
		ThisStdCall(0xA6E1F0, &result, pitch, roll, yaw);
		return result;
	}

	static NiQuaternion Slerp(const float fVal, NiQuaternion* start, NiQuaternion* end) {
		NiQuaternion result{};
		ThisStdCall(0xA6E330, &result, fVal, start, end);
		return result;
	}

	NiMatrix3 ToRotation() const {
		NiMatrix3 result{};
		ThisStdCall(0x4F0180, this, &result);
		return result;
	}

	NiQuaternion& operator/=(const float& rhs) {
		m_fW /= rhs;
		m_fX /= rhs;
		m_fY /= rhs;
		m_fZ /= rhs;
		return *this;
	}

	bool operator==(const NiQuaternion& rhs) const {
		return m_fW == rhs.m_fW && m_fX == rhs.m_fX && m_fY == rhs.m_fY && m_fZ == rhs.m_fZ;
	}

	bool operator!=(const NiQuaternion& rhs) const {
		return m_fW != rhs.m_fW || m_fX != rhs.m_fX || m_fY != rhs.m_fY || m_fZ != rhs.m_fZ;
	}

	NiQuaternion& operator=(const NiQuaternion& rhs) {
		m_fW = rhs.m_fW;
		m_fX = rhs.m_fX;
		m_fY = rhs.m_fY;
		m_fZ = rhs.m_fZ;
		return *this;
	}

	NiQuaternion& operator=(const float& rhs) {
		m_fW = rhs;
		m_fX = rhs;
		m_fY = rhs;
		m_fZ = rhs;
		return *this;
	}

	NiPoint3 ToEulerAngles() {
		NiPoint3 angles;

		double sinr_cosp = 2 * (m_fW * m_fX + m_fY * m_fZ);
		double cosr_cosp = 1 - 2 * (m_fX * m_fX + m_fY * m_fY);
		angles.x = std::atan2(sinr_cosp, cosr_cosp);

		double sinp = std::sqrt(1 + 2 * (m_fW * m_fY - m_fX * m_fZ));
		double cosp = std::sqrt(1 - 2 * (m_fW * m_fY - m_fX * m_fZ));
		angles.y = 2 * std::atan2(sinp, cosp) - std::numbers::pi_v<float> / 2;

		double siny_cosp = 2 * (m_fW * m_fZ + m_fX * m_fY);
		double cosy_cosp = 1 - 2 * (m_fY * m_fY + m_fZ * m_fZ);
		angles.z = std::atan2(siny_cosp, cosy_cosp);

		return angles;
	}
};