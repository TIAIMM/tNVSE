#include "NiPoint3.hpp"

NiPoint3 const NiPoint3::UNIT_X = NiPoint3(1,0,0);
NiPoint3 const NiPoint3::UNIT_Y = NiPoint3(0,1,0);
NiPoint3 const NiPoint3::UNIT_Z = NiPoint3(0,0,1);
NiPoint3 const NiPoint3::UNIT_ALL = NiPoint3(1,1,1);
NiPoint3 const NiPoint3::ZERO = NiPoint3(0,0,0);

// 0xA59BE0
void NiPoint3::LoadBinary(NiStream& arStream) {
	ThisStdCall(0xA59BE0, this, &arStream);
}

// 0xA69780
void NiPoint3::SaveBinary(NiStream& arStream) const {
	ThisStdCall(0xA69780, this, &arStream);
}
