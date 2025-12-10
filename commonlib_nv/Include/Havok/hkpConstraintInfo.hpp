#pragma once

struct hkpConstraintInfo {
	SInt32 m_maxSizeOfSchema;
	SInt32 m_sizeOfSchemas;
	SInt32 m_numSolverResults;
	SInt32 m_numSolverElemTemps;
};

ASSERT_SIZE(hkpConstraintInfo, 0x10);