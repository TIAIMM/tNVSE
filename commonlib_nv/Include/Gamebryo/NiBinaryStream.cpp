#include "NiBinaryStream.hpp"

NiBinaryStream::NiBinaryStream() {
    m_pfnRead   = 0;
    m_pfnWrite  = 0;
    m_uiAbsoluteCurrentPos = 0;
}

UInt32 NiBinaryStream::GetPosition() const {
    return m_uiAbsoluteCurrentPos;
}

// 0xAA1DE0
void NiBinaryStream::DoByteSwap(void* apvData, UInt32 auiBytes, UInt32* apuiComponentSizes, UInt32 auiNumComponents) {
    CdeclCall(0xAA1DE0, apvData, auiBytes, apuiComponentSizes, auiNumComponents);
}
