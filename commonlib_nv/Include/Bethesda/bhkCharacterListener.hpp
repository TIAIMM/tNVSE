#pragma once

#include "hkpCharacterProxyListener.hpp"

class bhkCharacterListener : public hkpCharacterProxyListener {
public:
	enum Flags : UInt32 {
		UNK_1				= 1u << 0,
		UNK_2				= 1u << 1,
		UNK_4				= 1u << 2,
		UNK_8				= 1u << 3,
		UNK_10				= 1u << 4,
		UNK_20				= 1u << 5,
		UNK_40				= 1u << 6,
		UNK_80				= 1u << 7,
		UNK_100				= 1u << 8,
		UNK_200				= 1u << 9,
		NOT_JUMPING			= 1u << 10,
		DISABLED_COLLISION	= 1u << 11,
		UNK_1000			= 1u << 12,
		UNK_2000			= 1u << 13,
		UNK_4000			= 1u << 14, // No speed mult?
		DEBUG_GEOMETRY		= 1u << 15,
		UNK_10000			= 1u << 16,
		UNK_20000			= 1u << 17,
		UNK_40000			= 1u << 18, // In LOD range?
		IS_PROJECTILE		= 1u << 19,
		COMPLEX_SCENE		= 1u << 20,
		UNK_200000			= 1u << 21,
		UNK_400000			= 1u << 22,
		UNK_800000			= 1u << 23,
		UNK_1000000			= 1u << 24,
		TILT_FRONT_BACK		= 1u << 25,
		TILT_LEFT_RIGHT		= 1u << 26,
		USING_FURNITURE		= 1u << 27,
		UNK_10000000		= 1u << 28,
		UNK_20000000		= 1u << 29,
		UNK_40000000		= 1u << 30,
		UNK_80000000		= 1u << 31,
	};

	Bitfield32	uiFlags;
	UInt32		unk008;
	UInt32		unk00C;
	hkVector4	unk010;
	float		fCosMaxSlopeAngle;
	UInt32		eMaterial024;
	UInt32		eLayerType;
	UInt32		unk02C;
	hkVector4	unk030;
	hkVector4	hkSupportNorm;
	float		unk050;
	float		unk054;
	float		unk058;
	float		unk05C;
	float		fScaledHeight_060;
	float		fScaledHeightOverSinTimeCosMaxAngle;
	bool		byte068;
	bool		byte069;
	UInt32		uiContactPoints;

	void SetVelocity(const NiPoint3& arVelocity);
};

ASSERT_SIZE(bhkCharacterListener, 0x70);