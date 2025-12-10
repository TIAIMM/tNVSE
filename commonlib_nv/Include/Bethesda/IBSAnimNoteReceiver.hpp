#pragma once

class BSAnimNote;

class IBSAnimNoteReceiver {
public:
	virtual ~IBSAnimNoteReceiver();
	virtual void Update(BSAnimNote* apNote);
};