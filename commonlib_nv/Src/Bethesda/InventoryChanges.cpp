#include "InventoryChanges.hpp"
#include "Actor.hpp"
#include "AutoMemContext.hpp"

TESObjectREFR** const InventoryChanges::pScriptRef = reinterpret_cast<TESObjectREFR** const>(0x11C6444);

// GAME - 0x4BEFB0
InventoryChanges::InventoryChanges(TESObjectREFR* apOwner) {
	ThisStdCall(0x4BEFB0, this, apOwner);
}

// GAME - 0x4BF150
InventoryChanges::~InventoryChanges() {
	ThisStdCall(0x4BF150, this);
}

// GAME - 0x4D1960
// void InventoryChanges::InitScripts() {
// 	ThisStdCall(0x4D1960, this);
// }
