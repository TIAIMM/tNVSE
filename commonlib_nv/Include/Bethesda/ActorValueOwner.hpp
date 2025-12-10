#pragma once

#include "ActorValue.hpp"

class Actor;

class ActorValueOwner {
public:
	ActorValueOwner();
	~ActorValueOwner();

	virtual UInt32	GetBaseActorValueI(ActorValue::Index aeIndex);
	virtual float	GetBaseActorValueF(ActorValue::Index aeIndex);
	virtual UInt32	GetActorValueI(ActorValue::Index aeIndex);
	virtual float	GetActorValueF(ActorValue::Index aeIndex);
	virtual float	Fn_04(ActorValue::Index aeIndex);
	virtual float	Fn_05(void* arg);
	virtual float	GetModifier(ActorValue::Index aeIndex);
	virtual UInt32	GetPermanentActorValueI(ActorValue::Index aeIndex);
	virtual float	GetPermanentActorValueF(ActorValue::Index aeIndex);
	virtual Actor*	GetActor();
	virtual UInt16	GetLevel();
};

ASSERT_SIZE(ActorValueOwner, 0x4);