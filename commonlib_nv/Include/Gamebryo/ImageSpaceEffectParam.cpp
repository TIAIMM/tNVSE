#include "ImageSpaceEffectParam.hpp"

// GAME - 0x4EBA20
ImageSpaceEffectParam* ImageSpaceEffectParam::Create(ImageSpaceEffectParam* apThis) {
    return ThisStdCall<ImageSpaceEffectParam*>(0x4EBA20, apThis);
}
