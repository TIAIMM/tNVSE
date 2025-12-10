#pragma once

#include "ActorPathingMessageQueue.hpp"
#include "PathingLocation.hpp"
#include "PathingRequest.hpp"
#include "PathingSolution.hpp"

class DetailedActorPathHandler;
class Actor;

class ActorMover {
public:
	ActorMover();
	
	virtual				~ActorMover();
	virtual void		SetMovementFlag(UInt16);
	virtual void		ClearMovementFlag();
	virtual void		SetMovementFlags(UInt16 movementFlags);
	virtual void		Unk_04();
	virtual void		Update(float afDelta);
	virtual void		HandleTurnAnimationTimer(float timePassed);
	virtual void		Unk_07();
	virtual UInt32		GetMovementFlags() const;
	virtual void		GetOverrideMovementVector();
	virtual void		SaveGame();
	virtual void		LoadGame();
	virtual void		Unk_0C();
	virtual void		Unk_0D();
	virtual void		Reset();

	enum MovementFlags {
		kMoveFlag_Forward		= 1 << 0,
		kMoveFlag_Backward		= 1 << 1,
		kMoveFlag_Left			= 1 << 2,
		kMoveFlag_Right			= 1 << 3,
		kMoveFlag_TurnLeft		= 1 << 4,
		kMoveFlag_TurnRight		= 1 << 5,
		kMoveFlag_NonController = 1 << 6,
		//						  1 << 7
		kMoveFlag_Walking		= 1 << 8,
		kMoveFlag_Running		= 1 << 9,
		kMoveFlag_Sneaking		= 1 << 10,
		kMoveFlag_Swimming		= 1 << 11,
		kMoveFlag_Jump			= 1 << 12,
		kMoveFlag_Flying		= 1 << 13,
		kMoveFlag_Fall			= 1 << 14,
		kMoveFlag_Slide			= 1 << 15,
	};


	NiPoint3						kLookAtPosition;
	NiPoint3						kOverrideMovement;
	PathingRequestPtr				spPathingRequest;			
	PathingSolutionPtr				spPathingSolution;
	DetailedActorPathHandler*		pPathHandler;	
	Actor*							pActor;							
	UInt32							uiDoorRefIDForPathing;				
	ActorPathingMessageQueuePtr		spPathingMsgQueue;
	UInt32							uiMovementFlags1;
	UInt32							unk38;							
	UInt32							uiMovementFlags2;
	UInt16							wrd40;							
	UInt16							wrd42;							
	PathingLocation					kPathingLocation;		
	UInt32							unk6C;							
	bool							bPathingFailed;
	UInt8							byte71;							
	UInt8							byte72;							
	bool							bHasLookAtTarget;
	bool							bIsWaitingOnPath;
	UInt8							byte75;							
	UInt8							byte76;							
	bool							bIsOverrideMovement;				
	UInt8							byte78;							
	UInt32							uiTime7C;							
	UInt32							uiTime80;							
	UInt32							uiCount84;		

	void StopMoving();
};

ASSERT_SIZE(ActorMover, 0x88);