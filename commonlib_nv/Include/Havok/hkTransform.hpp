#pragma once

#include "hkVector4.hpp"
#include "hkRotation.hpp"

class hkTransform {
public:
	hkTransform() {}
	hkTransform(const hkTransform& aTransform) : rotation(aTransform.rotation), translation(aTransform.translation) {}
	hkTransform(const hkRotation& aRotation, const hkVector4& aTranslation) : rotation(aRotation), translation(aTranslation) {}
	~hkTransform() {}

	hkRotation	rotation;
	hkVector4	translation;

	hkVector4& getTranslation();
	const hkVector4& getTranslation() const;

	hkRotation& getRotation();
	const hkRotation& getRotation() const;
};

ASSERT_SIZE(hkTransform, 0x40)