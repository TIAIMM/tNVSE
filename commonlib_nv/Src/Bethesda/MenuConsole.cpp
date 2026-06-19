#include "MenuConsole.hpp"

// 0x71B160
MenuConsole* MenuConsole::GetSingleton() {
	return CdeclCall<MenuConsole*>(0x71B160, true);
}

char* MenuConsole::GetConsoleOutputFilename() {
	return GetSingleton()->COFileName;
};

bool MenuConsole::HasConsoleOutputFilename() {
	return 0 != GetSingleton()->COFileName[0];
};

void MenuConsole::Print(const char* apFormat, ...) {
	va_list args;
	va_start(args, apFormat);
	ThisStdCall(0x71D0A0, this, apFormat, args);
	va_end(args);
}
bool MenuConsole::IsConsoleActive() const {
	return ucConsoleState > 0;
}
bool MenuConsole::ToggleConsole() {
	return ThisStdCall(0x71D580, this);
}
;