#pragma once

#include "NiTList.hpp"
#include "BSString.hpp"

static const UInt32 s_Console__Print = 0x0071D0A0;

class MenuConsole {
public:
	MenuConsole();
	~MenuConsole();

	struct RecordedCommand {
		char buf[100];
	};


	void*				pScriptContext;
	NiTList<BSString>	lPrintedLines;
	NiTList<BSString>	lInputHistory;
	UInt32				uiHistoryIndex;
	UInt32				uiUnk020;
	UInt32				uiPrintedCount;
	UInt32				uiUnk028;
	UInt32				uiLineHeight;
	SInt32				iTextXPos;
	SInt32				iTextYPos;
	UInt8				ucConsoleState;
	bool				bUnk39;
	bool				bIsBatchRecording;
	bool				bUnk3B;
	UInt32				uiNumRecordedCommands;
	RecordedCommand		recordedCommands[20];
	char				COFileName[260];

	static MenuConsole* GetSingleton();

	static char* GetConsoleOutputFilename();
	static bool HasConsoleOutputFilename();

	void Print(const char* apFormat, ...);
	bool IsConsoleActive() const;
	bool ToggleConsole();
};

ASSERT_SIZE(MenuConsole, 0x914u);