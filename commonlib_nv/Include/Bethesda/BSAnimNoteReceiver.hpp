#pragma once

#include "IBSAnimNoteReceiver.hpp"
#include "BSAnimNote.hpp"
#include "NiTArray.hpp"

class Actor;

class BSAnimNoteReceiver : public IBSAnimNoteReceiver {
public:
	struct BSAnimNoteReceiverType {
		BSAnimNote::Type	eType;
		void*				pCallback;
	};

	Actor* pActor;
	NiTPrimitiveArray<BSAnimNoteReceiverType*> kReceiverTypes;
};
