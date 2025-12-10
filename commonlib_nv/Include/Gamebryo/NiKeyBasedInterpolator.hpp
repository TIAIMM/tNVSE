#pragma once

#include "NiInterpolator.hpp"
#include "NiAnimationKey.hpp"

class NiKeyBasedInterpolator : public NiInterpolator {
public:
	NiKeyBasedInterpolator();
	virtual ~NiKeyBasedInterpolator();

    virtual UInt16                      GetKeyChannelCount() const;
    virtual UInt32                      GetKeyCount(UInt16 ausChannel) const;
    virtual NiAnimationKey::KeyType     GetKeyType(UInt16 ausChannel) const;
    virtual NiAnimationKey::KeyContent  GetKeyContent(UInt16 ausChannel) const;
    virtual NiAnimationKey*             GetKeyArray(UInt16 ausChannel) const;
    virtual unsigned char               GetKeyStride(UInt16 ausChannel) const;
    virtual bool                        GetChannelPosed(UInt16 ausChannel) const;

	NIRTTI_ADDRESS(0x11F3740);

    UInt32 GetAllocatedSize(UInt16 ausChannel) const;
};