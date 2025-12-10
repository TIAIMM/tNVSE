#include "BGSSaveLoadGame.hpp"
#include "BGSLoadGameBuffer.hpp"
#include "BGSSaveGameBuffer.hpp"
#include "TESFile.hpp"

BGSSaveLoadGame* BGSSaveLoadGame::GetSingleton() {
    return *(BGSSaveLoadGame**)0x11DDF38;
}

// GAME - 0x847660
bool BGSSaveLoadGame::LoadPluginList(BGSSaveLoadFile* apFile) {
	return ThisStdCall<bool>(0x847660, this, apFile);
}

// GAME - 0x847590
void BGSSaveLoadGame::SavePluginList(BGSSaveLoadFile* apFile) {
	ThisStdCall(0x847590, this, apFile);
}

// Inlined at 0x846D80
UInt32 BGSSaveLoadGame::GetConvertedFormID(UInt32 auiFormID) {
	return ThisStdCall<UInt32>(0x846D80, this, auiFormID);
}

// GAME - 0x846DE0
UInt8 BGSSaveLoadGame::GetSaveMod(UInt8 aucIndex) const {
	return ucSaveMods[aucIndex];
}

// GAME - 0x42CE10
bool BGSSaveLoadGame::IsLoading() {
    return (uiGlobalFlags & 2) != 0;
}

// GAME - 0x546950
bool BGSSaveLoadGame::IsDeferInitForms() {
    return (uiGlobalFlags & 0x10) != 0;
}

bool BGSSaveLoadGame::SetThreadAllowChanges(bool abEnable) {
    return ThisStdCall<bool>(0x4623F0, this, abEnable);
}

// GAME - 0x8492B0
void BGSSaveLoadGame::InitForms(bool abLowPriority) {
    ThisStdCall(0x8492B0, this, abLowPriority);
}

void BGSSaveLoadGame::LoadCell(TESObjectCELL* apCell) {
    ThisStdCall(0x849B10, this, apCell);
}
