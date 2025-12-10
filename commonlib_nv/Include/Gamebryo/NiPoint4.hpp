#pragma once

#include "NiRect.hpp"
#include "NiPoint3.hpp"

class NiPoint4 : public BSMemObject {
public:
	float x;
	float y;
	float z;
	float w;

	NiPoint4() : x(0.f), y(0.f), z(0.f), w(0.f) {};
	NiPoint4(const float x, const float y, const float z, const float w) : x(x), y(y), z(z), w(w) {};
	NiPoint4(const NiPoint3& src) : x(src.x), y(src.y), z(src.z), w(0.f) {};
	NiPoint4(const NiPoint3& src, float w) : x(src.x), y(src.y), z(src.z), w(w) {};

	auto operator<=>(const NiPoint4&) const = default;

	inline const float operator[] (UInt32 i) const { return ((float*)&x)[i]; };
	inline float operator[] (UInt32 i) { return ((float*)&x)[i]; };

	operator float* () const { return (float*)this; };

	operator NiRect<float>() const {
		return NiRect<float>(x, y, z, w);
	};

	NiPoint4 operator=(NiPoint3 pt) const {
		return NiPoint4(pt.x, pt.y, pt.z, w);
	};

	void operator=(NiPoint3* pt) {
		x = pt->x;
		y = pt->y;
		z = pt->z;
	};

	void operator=(const NiPoint3& pt) {
		x = pt.x;
		y = pt.y;
		z = pt.z;
	};

	NiPoint4 operator-(NiPoint3& pt) const {
		return NiPoint4(x - pt.x, y - pt.y, z - pt.z, w);
	};

	NiPoint4 operator-(NiPoint3* pt) const {
		return NiPoint4(x - pt->x, y - pt->y, z - pt->z, w);
	};

	NiPoint4* operator-(const NiPoint3& pt) const {
		return new NiPoint4(x - pt.x, y - pt.y, z - pt.z, w);
	};

	NiPoint4* operator-(const NiPoint3* pt) const {
		return new NiPoint4(x - pt->x, y - pt->y, z - pt->z, w);
	};

	NiPoint4 operator+ (const NiPoint4& pt) const { return NiPoint4(x + pt.x, y + pt.y, z + pt.z, w + pt.w); };
	NiPoint4& operator+= (const NiPoint4& pt) {
		x += pt.x;
		y += pt.y;
		z += pt.z;
		w += pt.w;
		return *this;
	};

	NiPoint4 operator*(const float afScalar) {
		return NiPoint4(x * afScalar, y * afScalar, z * afScalar, w * afScalar);
	}

	NiPoint4& operator*= (float afScalar) {
		x *= afScalar;
		y *= afScalar;
		z *= afScalar;
		w *= afScalar;
		return *this;
	};

	static const NiPoint4 ZERO;
};

ASSERT_SIZE(NiPoint4, 0x10);

typedef __declspec(align(16)) NiPoint4 NiPoint4A;