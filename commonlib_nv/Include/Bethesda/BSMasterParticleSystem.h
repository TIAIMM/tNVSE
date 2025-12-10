#pragma once

#include "NiTArray.hpp"
#include "NiNode.hpp"
#include "NiTList.hpp"

struct BSMasterParticleSystem : NiNode {
    NiTList<NiAVObjectPtr> kEmitterObjList;
    UInt16 usActiveEmitterObjCount;
    UInt16 usMaxEmitterObj;
    UInt16 usMasterParticleCap;
    NiAVObjectPtr* kEmitterIterator;
    UInt32 uiIndex;
    UInt32 uiNodeIndex;
    NiTPrimitiveArray<NiParticles*> kChildParticles;
    float fMasterPSysTime;
};
