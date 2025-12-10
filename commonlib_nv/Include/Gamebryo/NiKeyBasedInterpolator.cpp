#include "NiKeyBasedInterpolator.hpp"
#include "NiAnimationKey.hpp"
#include "NiEulerRotKey.hpp"

UInt32 NiKeyBasedInterpolator::GetAllocatedSize(UInt16 ausChannel) const {
    if (GetKeyType(ausChannel) == NiAnimationKey::EULERKEY) {
	    const auto pkEulerKey = reinterpret_cast<NiEulerRotKey*>(GetKeyArray(ausChannel));
        UInt32 uiSize = 0;

        if (!pkEulerKey)
            return uiSize;

        uiSize = sizeof(NiEulerRotKey);
        for (UInt32 ui = 0; ui < 3; ui++) {
            UInt32 uiNumKeys = pkEulerKey->GetNumKeys(ui);
            UInt32 uiKeySize = pkEulerKey->GetKeySize(ui);
            uiSize += uiNumKeys * uiKeySize;
        }

        return uiSize;
    }

    return GetKeyCount(ausChannel) * GetKeyStride(ausChannel);
}
