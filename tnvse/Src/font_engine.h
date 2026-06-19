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
	static constexpr UInt32 kTlsIndexAddr = 0x126FD98;
	static constexpr UInt32 kTlsSlotValue = 12;
	static constexpr int kTlsByteOffset = 692;
	static constexpr UInt32 kFontDataSize = 0x3928;
	static constexpr UInt32 kMaxGlyphCount = 256;
	static constexpr UInt32 kExtraGlyphReserve = 24066;
	static constexpr UInt32 kSentinelMax = 0x7FFFFFFF;
	static constexpr int kTabWidth = 75;
	static constexpr UInt8 kSpaceChar = 32;
	static constexpr UInt8 kNBSPChar = 160;
	static constexpr UInt8 kDelChar = 127;
	static constexpr UInt8 kPipeChar = 124;

	// ---- RAII guard for TLS slot management ----
	class TlsSlotGuard
	{
		UInt32 m_savedValue;
		UInt32* m_target;
	public:
		TlsSlotGuard()
		{
			UInt32* pTlsIndex = (UInt32*)kTlsIndexAddr;
			UInt32 tebAddress;
			__asm {
				mov eax, fs: [0x18]
				mov tebAddress, eax
			}
			UInt32 tlsPointer = *(UInt32*)(tebAddress + 0x2C);
			UInt32 tlsSlotAddress = *(UInt32*)(tlsPointer + (*pTlsIndex) * 4);
			m_target = (UInt32*)(tlsSlotAddress + kTlsByteOffset);
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
			const NiColorA* axFontColor, NiTriShape** apTextShape, NiTriShape** apIconShape);
		UInt32* MakeString(
			float afStartX, float afStartY, float afZ,
			BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
			const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1);

	private:
		void LoadExtraGlyphs(BSFile* fontFile, UInt32* textureMarkers);
		float ComputeGlyphMetrics();
		bool LoadFontTextures(UInt32* textureMarkers, int& stringRefFlag);
	};

} // namespace fonthook
