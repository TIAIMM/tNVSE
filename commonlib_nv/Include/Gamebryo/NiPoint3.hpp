#pragma once

#include "BSMemObject.hpp"
#include <numbers>

class NiStream;

class NiPoint3 : public BSMemObject {
public:
	float x;
	float y;
	float z;
	

	NiPoint3() : x(0.f), y(0.f), z(0.f) {};
	NiPoint3(const float x, const float y, const float z) : x(x), y(y), z(z) {};
	explicit NiPoint3(const float f) : x(f), y(f), z(f) {};
	NiPoint3(const int x, const int y, const int z) : x(x), y(y), z(z) {};
	explicit NiPoint3(const int f) : x(f), y(f), z(f) {};
	NiPoint3(const NiPoint3& src) : x(src.x), y(src.y), z(src.z) {};
	explicit NiPoint3(const NiPoint3* src) : x(src->x), y(src->y), z(src->z) {};
	
	auto operator<=>(const NiPoint3&) const = default;

	bool operator==(const NiPoint3& pt) const {
		return x == pt.x && y == pt.y && z == pt.z;
	}

#ifdef __D3DX9MATH_INL__
	D3DXVECTOR3 AsD3DXVECTOR3() const { return (D3DXVECTOR3)*this; };
#endif

	inline const float operator[] (UInt32 i) const { return ((float*)&x)[i]; };
	inline float operator[] (UInt32 i) { return ((float*)&x)[i]; };

	operator float* () const { return (float*)this; };

	// Point operations

	NiPoint3& operator= (const NiPoint3& pt) {
		x = pt.x;
		y = pt.y;
		z = pt.z;
		return *this;
	}

	NiPoint3 operator+ (const NiPoint3& pt) const { return NiPoint3(x + pt.x, y + pt.y, z + pt.z); };
	NiPoint3& operator+= (const NiPoint3& pt) {
		x += pt.x;
		y += pt.y;
		z += pt.z;
		return *this;
	};

	NiPoint3 operator- (const NiPoint3& pt) const { return NiPoint3(x - pt.x, y - pt.y, z - pt.z); };
	NiPoint3 operator- () const { return NiPoint3(-x, -y, -z); };
	NiPoint3& operator-= (const NiPoint3& pt) {
		x -= pt.x;
		y -= pt.y;
		z -= pt.z;
		return *this;
	};

	float operator* (const NiPoint3& pt) const { return x * pt.x + y * pt.y + z * pt.z; };

	// Scalar operations

	NiPoint3 operator* (float fScalar) const { return NiPoint3(fScalar * x, fScalar * y, fScalar * z); };

	friend NiPoint3 operator* (float fScalar, const NiPoint3& pt) { return NiPoint3(fScalar * pt.x, fScalar * pt.y, fScalar * pt.z); };

	NiPoint3& operator*= (float fScalar) {
		x *= fScalar;
		y *= fScalar;
		z *= fScalar;
		return *this;
	};



	NiPoint3 operator/ (float fScalar) const {
		float fInvScalar = 1.0f / fScalar;
		return NiPoint3(fInvScalar * x, fInvScalar * y, fInvScalar * z);
	};

	NiPoint3& operator/= (float fScalar) {
		x /= fScalar;
		y /= fScalar;
		z /= fScalar;
		return *this;
	};

	// 0x457990
	__forceinline float Length() const {
		return std::sqrt(x * x + y * y + z * z);
	}

	__forceinline float SqrLength() const {
		return x * x + y * y + z * z;
	}

	__forceinline float Dot(const NiPoint3& pt) const {
		return x * pt.x + y * pt.y + z * pt.z;
	}

	__forceinline NiPoint3 Cross(const NiPoint3& pt) const {
		return NiPoint3(y * pt.z - z * pt.y, z * pt.x - x * pt.z, x * pt.y - y * pt.x);
	}

	__forceinline NiPoint3 Normalize() const {
		return *this / this->Length();
	}

	__forceinline NiPoint3 AsAngle() const {
		return {
			asin(-z),
			0.f,
			atan2(x, y)
		};
	}

	__forceinline NiPoint3 ToDegrees() const {
		return {
			x * (180.0f / static_cast<float>(std::numbers::pi)),
			y * (180.0f / static_cast<float>(std::numbers::pi)),
			z * (180.0f / static_cast<float>(std::numbers::pi))
		};
	}

	__forceinline float Unitize() {
		float fLength = Length();

		if (fLength > 1e-06f) {
			float fRecip = 1.0f / fLength;
			x *= fRecip;
			y *= fRecip;
			z *= fRecip;
		}
		else
		{
			x = 0.0f;
			y = 0.0f;
			z = 0.0f;
			fLength = 0.0f;
		}
		return fLength;
	}

	inline NiPoint3 UnitCross(const NiPoint3& pt) const {
		NiPoint3 cross(y * pt.z - z * pt.y, z * pt.x - x * pt.z, x * pt.y - y * pt.x);
		float fLength = cross.Length();
		if (fLength > 1e-06f)
			return cross / fLength;
		else
			return NiPoint3(0.0f, 0.0f, 0.0f);
	}

	inline float Distance(const NiPoint3& pt) const {
		return (*this - pt).Length();
	}

	static void PointsPlusEqualFloatTimesPoints(NiPoint3* pkDst, float f, const NiPoint3* pkSrc, unsigned int uiVerts) {
		for (UInt32 i = 0; i < uiVerts; i++) {
			pkDst[i] += f * pkSrc[i];
		}
	}

	void Lerp(const NiPoint3& to, const NiPoint3& from, const double by) {
		x = to.x * (1 - by) + from.x * by;
		y = to.y * (1 - by) + from.y * by;
		z = to.z * (1 - by) + from.z * by;
	}

	NiPoint3& Lerp(const NiPoint3& to, const NiPoint3& from, const NiPoint3& by) {
		x = to.x * (1 - by.x) + from.x * by.x;
		y = to.y * (1 - by.y) + from.y * by.y;
		z = to.z * (1 - by.z) + from.z * by.z;
		return *this;
	}

	float GetLargest() const {
		float largest = x;
		if (y > largest)
			largest = y;
		if (z > largest)
			largest = z;
		return largest;
	}

	static float Sign(NiPoint3 p1, NiPoint3 p2, NiPoint3 p3) {
		return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
	}

	static bool PointInTriangle(NiPoint3 pt, NiPoint3 v1, NiPoint3 v2, NiPoint3 v3) {
		bool b1 = Sign(pt, v1, v2) < 0.0;
		bool b2 = Sign(pt, v2, v3) < 0.0;
		bool b3 = Sign(pt, v3, v1) < 0.0;

		return (b1 == b2) && (b2 == b3);
	}

	static NiPoint3 GetTriangleCenter(NiPoint3 v1, NiPoint3 v2, NiPoint3 v3) {
		return NiPoint3((v1.x + v2.x + v3.x) / 3.0f, (v1.y + v2.y + v3.y) / 3.0f, (v1.z + v2.z + v3.z) / 3.0f);
	}

	void LoadBinary(NiStream& arStream);
	void SaveBinary(NiStream& arStream) const;

	NiPoint3& Reset() {
		*this = ZERO;
		return *this;
	}

	static const NiPoint3 UNIT_X;
	static const NiPoint3 UNIT_Y;
	static const NiPoint3 UNIT_Z;
	static const NiPoint3 UNIT_ALL;
	static const NiPoint3 ZERO;
};

ASSERT_SIZE(NiPoint3, 0xC);

typedef __declspec(align(16)) NiPoint3 NiPoint3A;