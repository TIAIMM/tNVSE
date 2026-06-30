#include "NiGlobalStringTable.hpp"

// 0xA5B690
NiGlobalStringTable::GlobalStringHandle NiGlobalStringTable::AddString(const char* pcString) {
    return CdeclCall<GlobalStringHandle>(0xA5B690, pcString);
}

// 0x43BA60
void NiGlobalStringTable::IncRefCount(const GlobalStringHandle& arHandle) {
    if (!arHandle)
        return;

    InterlockedIncrement(reinterpret_cast<size_t*>(GetRealBufferStart(arHandle)));
}

// 0x4381D0
void NiGlobalStringTable::DecRefCount(const GlobalStringHandle& arHandle) {
    if (!arHandle)
        return;

    InterlockedDecrement(reinterpret_cast<size_t*>(GetRealBufferStart(arHandle)));
}

UInt32 NiGlobalStringTable::GetLength(const GlobalStringHandle& arHandle) {
    if (!arHandle)
		return 0;

    return GetRealBufferStart(arHandle)[1];
}

// 0x438210
char* NiGlobalStringTable::GetRealBufferStart(const GlobalStringHandle& arHandle) {
    return (static_cast<char*>(arHandle) - 2 * sizeof(size_t));
}
