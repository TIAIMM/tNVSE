#pragma once
#include "BSFile.hpp"
#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	// ---- Constants ----
	static constexpr DWORD kTlsIndexAddr = 0x126FD98;
	static constexpr DWORD kTlsSlotValue = 12;
	static constexpr int kTlsByteOffset = 692;
	static constexpr UInt32 kFontDataSize = 0x3928;
	static constexpr UInt32 kMaxGlyphCount = 256;
	static constexpr UInt32 kExtraGlyphReserve = 24066;
	static constexpr UInt32 kSentinelMax = 0x7FFFFFFF;
	static constexpr int kTabWidth = 75;
	static constexpr char kSpaceChar = 32;
	static constexpr char kNBSPChar = 160;
	static constexpr char kDelChar = 127;
	static constexpr char kPipeChar = 124;

	// ---- RAII guard for TLS slot management ----
	class TlsSlotGuard
	{
		DWORD m_savedValue;
		DWORD* m_target;
	public:
		TlsSlotGuard()
		{
			DWORD* pTlsIndex = (DWORD*)kTlsIndexAddr;
			DWORD tebAddress;
			__asm {
				mov eax, fs: [0x18]
				mov tebAddress, eax
			}
			DWORD tlsPointer = *(DWORD*)(tebAddress + 0x2C);
			DWORD tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
			m_target = (DWORD*)(tlsSlotAddress + kTlsByteOffset);
			m_savedValue = *m_target;
			*m_target = kTlsSlotValue;
		}
		~TlsSlotGuard()
		{
			*m_target = m_savedValue;
		}
		TlsSlotGuard(const TlsSlotGuard&) = delete;
		TlsSlotGuard& operator=(const TlsSlotGuard&) = delete;
	};

	// ---- FontEx - Extended font class with multi-byte support ----
	class FontEx : public Font
	{
	public:
		Font* FontInit(int iFontNum, char* apFilename, bool abLoad);
		void Load();
		void __thiscall PrepTextForTerminal(const char* apOrigString, Font::TextData* axData);
		void __thiscall PrepText(const char* apOrigString, Font::TextData* axData);
		UInt32 CreateText(
			BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
			int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
			const NiColorA* axFontColor, UINT32** apTextShape, UINT32** apIconShape);
		UInt32* MakeString(
			float afStartX, float afStartY, float afZ,
			BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
			const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1);

	private:
		void LoadExtraGlyphs(BSFile* fontFile, DWORD* textureMarkers);
		float ComputeGlyphMetrics();
		bool LoadFontTextures(DWORD* textureMarkers, int& stringRefFlag);
	};

} // namespace fonthook
