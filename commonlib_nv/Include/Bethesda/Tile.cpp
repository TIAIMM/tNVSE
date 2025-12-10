#include "Tile.hpp"

// GAME - 0xBF11A0
void Tile::SetValue(UInt32 auiID, const char* apString, bool abPropagate) {
	ThisStdCall(0xA01350, this, auiID, apString, abPropagate);
}

// GAME - 0xA012D0
void Tile::SetValue(UInt32 auiID, float afValue, bool abPropagate) {
	ThisStdCall(0xA012D0, this, auiID, afValue, abPropagate);
}

UInt32 Tile::TraitNameToID(const char* apTraitName) {
	return CdeclCall<UInt32>(0xA01860, apTraitName);
}

Tile::Value* Tile::GetValue(UInt32 auiID) {
	return ThisStdCall<Value*>(0xA01000, this, auiID);
}
