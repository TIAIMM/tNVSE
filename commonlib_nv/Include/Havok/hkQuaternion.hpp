#pragma once

class hkRotation;

__declspec(align(16)) class hkQuaternion {
public: 
	hkQuaternion(const hkRotation& arRotation);
	hkQuaternion(float ax, float ay, float az, float aw) : x(ax), y(ay), z(az), w(aw) {}
	hkQuaternion() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}

	float x;
	float y;
	float z;
	float w;

	void setSlerp(const hkQuaternion& q0, const hkQuaternion& q1, float t);

	static const hkQuaternion identity;
};

ASSERT_SIZE(hkQuaternion, 0x10);