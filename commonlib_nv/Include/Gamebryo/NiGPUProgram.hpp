#pragma once

#include "NiRefObject.hpp"

class NiGPUProgram : public NiRefObject {
public:
	NiGPUProgram();
	virtual ~NiGPUProgram();

	enum ProgramType : UInt32 {
		PROGRAM_VERTEX = 0,
		PROGRAM_PIXEL  = 2,
		PROGRAM_GEOMETRY,
		PROGRAM_MAX
	};

	union {
		ProgramType				m_eProgramType;

		struct {
			UInt8				ucProgramType;
			UInt8				empty[2];
			bool				bEnabled;
		}; // NVR
	};
};