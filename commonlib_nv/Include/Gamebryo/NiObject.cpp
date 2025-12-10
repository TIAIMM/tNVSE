#include "NiObject.hpp"
#include "NiRTTI.hpp"

// 0x6532C0
bool NiObject::IsKindOf(const NiRTTI& apRTTI) const {
    return GetRTTI()->IsKindOf(apRTTI);
}

// 0x6532C0
bool NiObject::IsKindOf(const NiRTTI* const apRTTI) const {
    return GetRTTI()->IsKindOf(apRTTI);
}

// 0x45BAF0
bool NiObject::IsExactKindOf(const NiRTTI* const apRTTI) const {
    return GetRTTI()->IsExactKindOf(apRTTI);
}

// 0x45BAF0
bool NiObject::IsExactKindOf(const NiRTTI& apRTTI) const {
    return GetRTTI()->IsExactKindOf(apRTTI);
}

// 0x45BAD0
bool NiObject::IsExactKindOf(const NiRTTI& apRTTI, NiObject* apObject) {
    return apObject && apObject->IsExactKindOf(apRTTI);
}

// 0x45BAD0
bool NiObject::IsExactKindOf(const NiRTTI* const apRTTI, NiObject* apObject) {
    return apObject && apObject->IsExactKindOf(apRTTI);
}