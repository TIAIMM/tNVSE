#include "PlayerCharacter.hpp"

#include "TESObjectWEAP.hpp"

NiCamera* PlayerCharacter::GetCamera1st() {
	return *(NiCamera**)0x11E07D0;
}

NiCamera* PlayerCharacter::GetCamera3rd() {
	return *(NiCamera**)0x11E07D4;
}

float PlayerCharacter::GetAttacksPerSecond() {
	const auto weap = GetEquippedWeapon();
	if (!weap) {
		return -1;
	}

	const auto weapInfo = pkBaseProcess->GetWeaponInfo();
	const auto shotsPerSec = CdeclCall<double>(0x645DC0, this, weap, 0, weapInfo);
	return static_cast<float>(shotsPerSec);
}