#include "hkVector4.hpp"

const hkVector4 hkVector4::zero = hkVector4(0.0f);

void hkVector4::setTransformedPos(const hkTransform& arTransform, const hkVector4& arVector) {
	ThisStdCall(0xC7F640, this, &arTransform, &arVector);
}

void hkVector4::setTransformedInversePos(const hkQsTransform& a, const hkVector4& b) {
	ThisStdCall(0xC7F840, this, &a, &b);
}

void hkVector4::setRotatedInverseDir(const hkQuaternion& a, const hkQuaternion& b) {
	ThisStdCall(0xC66320, this, &a, &b);
}
