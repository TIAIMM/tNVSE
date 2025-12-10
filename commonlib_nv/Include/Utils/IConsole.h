#pragma once
#include "ITypes.h"
#include "Memory.h"
#include <cstdarg>

class FontManager {
	FontManager();
	~FontManager();

public:
	void* pFontInfo;

	static FontManager* GetSingleton() {
		return CdeclCall<FontManager*>(0x5BD5B0, true);
	}
};

class ConsoleManager {
	void* scriptContext;
	uint8_t unk004[0x80C];
	char m_szOutputFileName[0x104];

protected:
	ConsoleManager();
	~ConsoleManager();

public:
	void Print(const char* fmt, const va_list args) {
		ThisStdCall(0x71D0A0, this, fmt, args);
	}

	static ConsoleManager* GetSingleton() {
		// Make sure FontManager is initialized
		const auto pFontMan = FontManager::GetSingleton();
		if (pFontMan && pFontMan->pFontInfo) {
			return CdeclCall<ConsoleManager*>(0x71B160, true);
		}

		return nullptr;
	}

	bool IsConsoleOpen() {
		return ThisStdCall<bool>(0x4A4020, this);
	}
};

static_assert(sizeof(ConsoleManager) == 0x914);

inline void Console_Print(const char* fmt, ...) {
	if (const auto manager = ConsoleManager::GetSingleton()) {
		va_list args;
		va_start(args, fmt);
		manager->Print(fmt, args);
		va_end(args);
	}
}
