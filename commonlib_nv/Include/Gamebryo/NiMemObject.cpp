#include "NiMemObject.hpp"
#include "NiMemManager.hpp"

// 0xAA13E0
void* NiMemObject::operator new(size_t stSize) {
    if (!stSize)
        stSize = 1;

    size_t stAlignment = 4;
    return NiMemManager::GetSingleton()->m_pkAllocator->Allocate(stSize, stAlignment, NI_OPER_NEW, true, 0, -1, 0);
}

// 0xAA1420
void* NiMemObject::operator new[](size_t stSize) {
    if (!stSize)
        stSize = 1;

    size_t stAlignment = 4;
    return NiMemManager::GetSingleton()->m_pkAllocator->Allocate(stSize, stAlignment, NI_OPER_NEW_ARRAY, false, 0, -1, 0);
}

// 0xAA1460
void NiMemObject::operator delete(void* pvMem, size_t stElementSize) {
    if (pvMem)
        NiMemManager::GetSingleton()->m_pkAllocator->Deallocate(pvMem, NI_OPER_DELETE, stElementSize);
}

// 0xAA1490
void NiMemObject::operator delete[](void* pvMem, size_t stElementSize) {
    if (pvMem)
        NiMemManager::GetSingleton()->m_pkAllocator->Deallocate(pvMem, NI_OPER_DELETE_ARRAY, -1);
}
