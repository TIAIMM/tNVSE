#pragma once

#include "hkArray.hpp"
#include "hkQsTransform.hpp"

class hkaSkeleton;

struct __declspec(align(16)) hkaSkeletonMapperData {
	struct SimpleMapping {
		SInt16 m_boneA;
		SInt16 m_boneB;
		hkQsTransform m_aFromBTransform;
	};

	struct ChainMapping {
		SInt16			m_startBoneA;
		SInt16			m_endBoneA;
		SInt16			m_startBoneB;
		SInt16			m_endBoneB;
		hkQsTransform	m_startAFromBTransform;
		hkQsTransform	m_endAFromBTransform;
	};

	hkaSkeleton*			m_skeletonA;
	hkaSkeleton*			m_skeletonB;
	hkArray<SimpleMapping>	m_simpleMappings;
	hkArray<ChainMapping>	m_chainMappings;
	hkArray<UInt16>			m_unmappedBones;
	hkQsTransform			m_extractedMotionMapping;
	bool					m_keepUnmappedLocal;
	UInt32					m_mappingType;
};

ASSERT_SIZE(hkaSkeletonMapperData, 0x70);