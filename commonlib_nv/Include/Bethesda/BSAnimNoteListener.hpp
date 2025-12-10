#pragma once

#include "NiTArray.hpp"
#include "BSAnimNote.hpp"
#include "BSAnimNoteReceiver.hpp"

class Actor;

class BSAnimNoteListener {
public:
	struct BSAnimReceiverType {
		BSAnimNote::Type	eNoteType;
		BSAnimNoteReceiver*	pReceiver;
	};

	NiTPrimitiveArray<BSAnimNoteListener::BSAnimReceiverType*>	kReceivers;
};