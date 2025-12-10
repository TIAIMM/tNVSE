#include "MobileObject.hpp"

bhkCharacterController* MobileObject::GetCharacterController() const {
    if (pkBaseProcess) {
        return pkBaseProcess->GetCharacterController();
    }
    
    return nullptr;
}