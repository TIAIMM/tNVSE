#pragma once

class NiAVObject;

class BSParticleSystemManager {
public:
	static BSParticleSystemManager* GetInstance() {
		return CdeclCall<BSParticleSystemManager*>(0x45A190);
	}

	NiAVObject* GetMasterParticleSystem(const uint32_t masterParticleSystemID) const {
		return ThisStdCall<NiAVObject*>(0xC50F30, this, masterParticleSystemID);
	}
};