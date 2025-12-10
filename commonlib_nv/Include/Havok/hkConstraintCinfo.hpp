#pragma once

#include "hkSerializableCinfo.hpp"

class NiStream;
class NiCloningProcess;
class NiViewerStringsArray;
class hkpConstraintData;

class hkConstraintCinfo : public hkSerializableCinfo {
public:
	virtual			~hkConstraintCinfo();
	virtual void	CloneData(hkpConstraintData* apCinfo, NiCloningProcess* apCloneProc) const;
	virtual void	LoadBinary(NiStream* apStream);
	virtual void	SaveBinary(NiStream* apStream);
	virtual UInt32	CreateData();
	virtual void	GetViewerStrings(NiViewerStringsArray* apStrings);
};

ASSERT_SIZE(hkConstraintCinfo, 0x4);