#include "InterfaceManager.hpp"
#include "PlayerCharacter.hpp"


// GAME - 0x702450
bool InterfaceManager::IsMenuActive(UInt32 auiMenuType) {
	return CdeclCall<bool>(0x702450, auiMenuType);
}

// GAME - 0x713FB0
void InterfaceManager::Render(int unused0, int unused1) {
	ThisStdCall(0x713FB0, this, unused0, unused1);
}
