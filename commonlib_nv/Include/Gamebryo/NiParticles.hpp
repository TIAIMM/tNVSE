#pragma once

#include "NiGeometry.hpp"

class NiQuaternion;
NiSmartPointer(NiParticles);

struct NiParticlesData : NiGeometryData
{
	bool bHasRotations;
	UInt16 *pusActiveIndices;
	float *pfVertexDots;
	float *m_pfRadii;
	UInt16 m_usActiveVertices;
	float *m_pfSizes;
	NiQuaternion *m_pkRotations;
	float *m_pfRotationAngles;
	NiPoint3 *m_pkRotationAxes;
	UInt8 *m_pucTextureIndices;
	void *m_pkSubTextureOffsets;
	UInt8 m_ucSubTextureOffsetCount;
};

class NiParticles : public NiGeometry {
public:
	NiParticles();
	virtual ~NiParticles();
};

ASSERT_SIZE(NiParticles, 0xC4)