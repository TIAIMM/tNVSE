#include "font_engine.h"
#include "font_manager.h"
#include "font_vector.h"
#include "font_vector_internal.h"
#include "game_hooks.h"
#include "hook_identity.h"
#include "hook_site.h"
#include "load_config.h"
#include "pipboy_search_icon_compat.h"
#include "SafeWrite.h"
#include "text_hooks.h"
#include "tnvse.h"

#include "TileText.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace fonthook
{
	namespace implementation::game_hooks {}
	using namespace implementation::game_hooks;

	namespace implementation::game_hooks
	{
		using hook_identity::Rel32Opcode;

		inline constexpr SIZE_T kFontConstructor = 0xA12020;
		inline constexpr SIZE_T kFontLoad = 0xA15320;
		inline constexpr SIZE_T kFontCreateText = 0xA12880;
		inline constexpr SIZE_T kFontMakeString = 0xA12460;
		inline constexpr SIZE_T kFontManagerCalculateStringDimensions = 0xA1B020;
		inline constexpr SIZE_T kFontPrepText = 0xA12FB0;

		// Font::Load allocates exactly sizeof(FontData), but retail asks BSFile for
		// the complete .FNT size and passes that value to Read.  Extended tNVSE
		// files append their DBCS FontLetter table, so the unbounded retail value
		// would overwrite the 0x3928-byte allocation before texture loading starts.
		//
		// At 0xA154D8 retail executes:
		//   mov edx, [eax+28h]     ; BSFile::GetSize virtual target
		//   call edx               ; __thiscall, ECX = BSFile*
		// Replace those five bytes with CALL rel32 to the explicit __fastcall
		// wrapper below.  A one-argument x86 __fastcall receives the same BSFile*
		// in ECX and returns the bounded UInt32 in EAX, so the surrounding retail
		// Font::Load code and its complete texture-loading path remain unchanged.
		struct FontLoadBoundedReadSizeHook
		{
			inline static constexpr SIZE_T kCallSite = 0xA154D8;
			inline static constexpr std::array<UInt8, 5>
				kExpectedGetSizeVirtualCallInstructions = {
				0x8B, 0x50, 0x28, // mov edx, dword ptr [eax+28h]
				0xFF, 0xD2,       // call edx
			};

			static UInt32 __fastcall GetBoundedFontDataReadSize(BSFile* fontFile)
			{
				const UInt32 fileSize = fontFile ? fontFile->GetSize() : 0;
				return std::min(fileSize, kFontDataSize);
			}

			static SIZE_T Target()
			{
				return reinterpret_cast<SIZE_T>(&GetBoundedFontDataReadSize);
			}

			static hook_site::InstructionCallSite& Site()
			{
				static hook_site::InstructionCallSite site{
					"Font::Load BSFile::GetSize instructions -> bounded reader (__fastcall, ECX=BSFile*)",
					kCallSite,
					kExpectedGetSizeVirtualCallInstructions,
					&GetBoundedFontDataReadSize
				};
				return site;
			}

			static bool HasVanillaInstructions()
			{
				return Site().MatchesOriginalBytes();
			}

			static bool IsInstalled()
			{
				return Site().IsInstalled();
			}

			static bool IsInstalledUnchecked()
			{
				return Site().IsInstalledUnchecked();
			}

			static bool ValidateVanilla()
			{
				if (HasVanillaInstructions())
					return true;
				gLog.FormattedMessage(
					"tnvse_font_hook: identity mismatch site=Font::Load BSFile::GetSize address=%08X expected=8B5028FFD2 (mov edx,[eax+28h]; call edx)",
					static_cast<UInt32>(kCallSite));
				return false;
			}

			static bool Install()
			{
				if (!ValidateVanilla())
					return false;

				WriteRelCall(kCallSite, &GetBoundedFontDataReadSize);
				if (IsInstalled())
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: installed Font::Load bounded FNT read site=%08X instruction=CALL rel32 target=%08X abi=__fastcall(ECX=BSFile*) maxBytes=%08X texturePath=retail",
						static_cast<UInt32>(kCallSite),
						static_cast<UInt32>(Target()),
						kFontDataSize);
					return true;
				}

				SIZE_T observedTarget = 0;
				const bool readable = hook_identity::ReadRel32Target(
					kCallSite, Rel32Opcode::Call, observedTarget);
				const bool restored = Rollback();
				gLog.FormattedMessage(
					"tnvse_font_hook: Font::Load bounded FNT read write verification failed site=%08X expected=%08X observed=%08X readable=%u rollback=%s",
					static_cast<UInt32>(kCallSite),
					static_cast<UInt32>(Target()),
					static_cast<UInt32>(observedTarget),
					readable ? 1u : 0u,
					restored ? "restored" : "incomplete");
				return false;
			}

			static bool Rollback()
			{
				if (Site().RollbackOwned())
					return true;

				SIZE_T observedTarget = 0;
				const bool readable = hook_identity::ReadRel32Target(
					kCallSite, Rel32Opcode::Call, observedTarget);
				gLog.FormattedMessage(
					"tnvse_font_hook: Font::Load bounded FNT read rollback retained site=%08X observed=%08X replacement=%08X readable=%u",
					static_cast<UInt32>(kCallSite),
					static_cast<UInt32>(observedTarget),
					static_cast<UInt32>(Target()),
					readable ? 1u : 0u);
				return false;
			}
		};

		void* __cdecl CopyAnimatingTextEncodedUnits(
			void* destination, const void* source, SIZE_T unitCount);

		std::array<hook_site::RelCallSite, 5> kCommonFontCallSites = {{
			{ "FontManager::CreateText -> FontManager::PrepText (__thiscall member)", 0xA18F4A, 0xA18A30, &FontManagerEx::PrepText },
			{ "FontManager::CreateText -> TextDoc::Render (__thiscall member)", 0xA18F63, 0xA19060, &FontManagerEx::TextDocRender },
			{ "TextDoc::Render -> Font::AddChar (__thiscall member)", 0xA19622, 0xA142D0, &FontEx::TextDocRenderAddChar },
			{ "Terminal text -> Font::PrepText (__thiscall member)", 0x759281, 0xA12FB0, &FontEx::PrepTextForTerminal },
			{ "TextLine wrap -> TextLine::AddChar (__thiscall member)", 0xA19C80, 0xA19F70, &FontManagerEx::TextLineAddChar },
		}};

		std::array<hook_site::RelCallSite, 1> kFreeTypeOnlyCallSites = {{
			{ "TextLine constructor -> TextLine::AddChar (__thiscall member)", 0xA1BDE2, 0xA19F70, &FontManagerEx::TextLineAddChar },
		}};

		std::array<hook_site::RelCallSite, 27> kMultibyteFontCallSites = {{
			{ "AnimatingText::Update -> encoded-unit memcpy (__cdecl)", 0x6FFFEE, 0x401460, &CopyAnimatingTextEncodedUnits },
			{ "FontManager::PrepText -> PrepHypertext (__thiscall member)", 0xA18ACC, 0xA17390, &FontManagerEx::PrepHypertext },
			{ "PrepHypertext CollectTo[0] (__fastcall thiscall shim)", 0xA1772D, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo[1] (__fastcall thiscall shim)", 0xA17835, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo[2] (__fastcall thiscall shim)", 0xA17A1E, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo[3] (__fastcall thiscall shim)", 0xA17B65, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo[4] (__fastcall thiscall shim)", 0xA17BB1, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo[5] (__fastcall thiscall shim)", 0xA17CFE, 0xA16EA0, &FontManagerEx::CollectTo },
			{ "PrepHypertext CollectTo attribute[0] (__fastcall thiscall shim)", 0xA17D5D, 0xA16EA0, &FontManagerEx::CollectToAttributeValue },
			{ "PrepHypertext CollectTo attribute[1] (__fastcall thiscall shim)", 0xA17DE9, 0xA16EA0, &FontManagerEx::CollectToAttributeValue },
			{ "FontManager::CreateText -> TextDoc::~TextDoc (__thiscall member)", 0xA18F7D, 0xA1B990, &FontManagerEx::TextDocDestructor },
			{ "PrepHypertext TextDoc::AddChar[0] (__thiscall member)", 0xA178A4, 0xA19A10, &FontManagerEx::TextDocAddChar },
			{ "PrepHypertext TextDoc::AddChar[1] (__thiscall member)", 0xA179D9, 0xA19A10, &FontManagerEx::TextDocAddChar },
			{ "PrepHypertext TextDoc::AddChar[2] (__thiscall member)", 0xA17FC2, 0xA19A10, &FontManagerEx::TextDocAddChar },
			{ "PrepText TextDoc::AddChar (__thiscall member)", 0xA18D7C, 0xA19A10, &FontManagerEx::TextDocAddChar },
			{ "TextDoc::AddChar -> TextPage::AddChar (__thiscall member)", 0xA19A6F, 0xA19C00, &FontManagerEx::TextPageAddChar },
			{ "TextPage constructor -> TextPage::AddChar (__thiscall member)", 0xA1BD1C, 0xA19C00, &FontManagerEx::TextPageAddChar },
			{ "PrepHypertext CharData::Copy[0] (__fastcall thiscall shim)", 0xA17898, 0xA1B660, &FontManagerEx::CharDataCopy },
			{ "PrepHypertext CharData::Copy[1] (__fastcall thiscall shim)", 0xA179CD, 0xA1B660, &FontManagerEx::CharDataCopy },
			{ "PrepHypertext CharData::Copy[2] (__fastcall thiscall shim)", 0xA17FB6, 0xA1B660, &FontManagerEx::CharDataCopy },
			{ "PrepText CharData::Copy (__fastcall thiscall shim)", 0xA18D73, 0xA1B660, &FontManagerEx::CharDataCopy },
			{ "Quest text -> Tile::SetString (__fastcall shim)", 0x77AF4B, 0xA01350, &TileSetStringHookForQuestAndLocationText },
			{ "Location text -> Tile::SetString (__fastcall shim)", 0x772B5E, 0xA01350, &TileSetStringHookForQuestAndLocationText },
			{ "Terminal text -> BSStringT<char>::c_str (__fastcall)", 0x7591AC, 0x559450, &BSString_c_strHook },
			{ "Location text -> BSStringT<char>::GetCStringOrEmpty (__fastcall)", 0x772B4B, 0x438EB0, &BSString_GetCStringOrEmptyHook },
			{ "Quest text -> strcpy_s[0] (__cdecl)", 0x77ACCC, 0x406D30, &strcpy_sHook },
			{ "Quest text -> strcpy_s[1] (__cdecl)", 0x77ACF8, 0x406D30, &strcpy_sHook },
		}};

		constexpr std::array<UInt8, 8> kFontPrepTextEntryPrefix = {
			0x55,                         // push ebp
			0x8B, 0xEC,                   // mov ebp, esp
			0x81, 0xEC, 0xE0, 0x07, 0x00, // first 5 bytes of sub esp, 000007E0h;
			                                 // final immediate byte 00 follows this prefix
		};
		hook_site::EntryJumpSite s_fontPrepTextEntrySite{
			"Font::PrepText entry (__thiscall member)",
			kFontPrepText,
			kFontPrepTextEntryPrefix,
			5,
			&FontEx::PrepText
		};

		template <size_t N>
		bool ValidateVanillaCallSites(
			const std::array<hook_site::RelCallSite, N>& sites)
		{
			bool valid = true;
			for (const hook_site::RelCallSite& site : sites)
			{
				SIZE_T actualTarget = 0;
				if (!site.ReadTarget(actualTarget))
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expected=CALL rel32",
						site.description,
						static_cast<UInt32>(site.callAddress));
					valid = false;
					continue;
				}
				if (actualTarget != site.expectedTarget)
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expectedTarget=%08X actualTarget=%08X",
						site.description,
						static_cast<UInt32>(site.callAddress),
						static_cast<UInt32>(site.expectedTarget),
						static_cast<UInt32>(actualTarget));
					valid = false;
				}
			}
			return valid;
		}

		template <size_t N>
		bool VerifyInstalledCallSites(
			const std::array<hook_site::RelCallSite, N>& sites)
		{
			bool installed = true;
			for (size_t i = 0; i < sites.size(); ++i)
			{
				SIZE_T observedTarget = 0;
				const bool readable = sites[i].ReadTarget(observedTarget);
				if (readable
					&& observedTarget == sites[i].replacementTarget)
					continue;
				gLog.FormattedMessage(
					"tnvse_font_hook: installed call verification failed site=%s address=%08X expected=%08X observed=%08X readable=%u",
					sites[i].description,
					static_cast<UInt32>(sites[i].callAddress),
					static_cast<UInt32>(sites[i].replacementTarget),
					static_cast<UInt32>(observedTarget),
					readable ? 1u : 0u);
				installed = false;
			}
			return installed;
		}

		template <size_t N>
		bool RollbackOwnedCallSites(
			std::array<hook_site::RelCallSite, N>& sites)
		{
			bool restored = true;
			for (size_t i = 0; i < sites.size(); ++i)
			{
				SIZE_T observedTarget = 0;
				if (sites[i].RollbackOwned(
						sites[i].expectedTarget, &observedTarget))
					continue;

				// Another top-level CALL may already retain tNVSE as a
				// predecessor. Never replace that later owner with vanilla.
				gLog.FormattedMessage(
					"tnvse_font_hook: call rollback retained site=%s address=%08X observed=%08X replacement=%08X",
					sites[i].description,
					static_cast<UInt32>(sites[i].callAddress),
					static_cast<UInt32>(observedTarget),
					static_cast<UInt32>(sites[i].replacementTarget));
				restored = false;
			}
			return restored;
		}

		bool VerifyFontPrepTextEntry(SIZE_T replacementTarget)
		{
			if (replacementTarget == s_fontPrepTextEntrySite.replacementTarget
				&& s_fontPrepTextEntrySite.IsInstalled())
			{
				return true;
			}
			SIZE_T observedTarget = 0;
			const bool readable = hook_identity::ReadRel32Target(
				kFontPrepText, Rel32Opcode::Jump, observedTarget);
			gLog.FormattedMessage(
				"tnvse_font_hook: Font::PrepText entry verification failed expected=%08X observed=%08X readable=%u",
				static_cast<UInt32>(replacementTarget),
				static_cast<UInt32>(observedTarget),
				readable ? 1u : 0u);
			return false;
		}

		bool RollbackOwnedFontPrepTextEntry(SIZE_T replacementTarget)
		{
			if (replacementTarget == s_fontPrepTextEntrySite.replacementTarget
				&& s_fontPrepTextEntrySite.RollbackOwned())
			{
				return true;
			}

			SIZE_T observedTarget = 0;
			hook_identity::ReadRel32Target(
				kFontPrepText, Rel32Opcode::Jump, observedTarget);
			gLog.FormattedMessage(
				"tnvse_font_hook: Font::PrepText rollback retained observed=%08X replacement=%08X",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(replacementTarget));
			return false;
		}

		bool ValidateRequiredFontHookSites()
		{
			bool valid = ValidateVanillaCallSites(kCommonFontCallSites);
			valid = FontLoadBoundedReadSizeHook::ValidateVanilla() && valid;
			if (g_bEnableMultibyteFontHook)
			{
				valid = ValidateVanillaCallSites(kMultibyteFontCallSites)
					&& valid;
				if (!s_fontPrepTextEntrySite.MatchesOriginalBytes())
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=Font::PrepText address=%08X length=%u",
						static_cast<UInt32>(kFontPrepText),
						static_cast<UInt32>(kFontPrepTextEntryPrefix.size()));
					valid = false;
				}
			}
			else
			{
				valid = ValidateVanillaCallSites(kFreeTypeOnlyCallSites)
					&& valid;
			}
			return valid;
		}

		using FontConstructorFn = Font* (__thiscall*)(Font*, int, char*, bool);
		using FontLoadFn = void (__thiscall*)(Font*);
		using FontCreateTextFn = void (__thiscall*)(Font*, BSStringT<char>*,
			int*, int*, int, int, int, char, const NiColorA*, NiTriShape**, NiTriShape**);
		using FontMakeStringFn = NiAVObject* (__thiscall*)(Font*, float, float,
			float, BSStringT<char>*, int*, bool, const NiColorA*, bool, bool);
		using CalculateStringDimensionsFn = NiPoint3* (__thiscall*)(FontManager*,
			NiPoint3*, const char*, UInt32, float, UInt32);

		FontConstructorFn s_originalFontConstructor = nullptr;
		FontLoadFn s_originalFontLoad = nullptr;
		FontCreateTextFn s_originalFontCreateText = nullptr;
		FontMakeStringFn s_originalFontMakeString = nullptr;
		CalculateStringDimensionsFn s_originalCalculateStringDimensions = nullptr;
		FontHookInstallState s_fontHookInstallState;
		bool s_fontHookGraphMismatchLogged = false;

		void* __cdecl CopyAnimatingTextEncodedUnits(
			void* destination, const void* source, SIZE_T unitCount)
		{
			const char* encodedSource = static_cast<const char*>(source);
			SIZE_T byteCount = 0;
			for (SIZE_T unitIndex = 0;
				unitIndex < unitCount && encodedSource[byteCount]; ++unitIndex)
			{
				UInt32 doubleByteCode = 0;
				byteCount += TryDecodeDoubleByte(
					encodedSource + byteCount, doubleByteCode) ? 2 : 1;
			}
			return std::memcpy(destination, source, byteCount);
		}

		constexpr std::array<UInt8, 5> kFontConstructorPrologue = {
			0x55,       // push ebp
			0x8B, 0xEC, // mov ebp, esp
			0x6A, 0xFF, // push -1
		};
		constexpr std::array<UInt8, 5> kFontLoadPrologue = {
			0x55,       // push ebp
			0x8B, 0xEC, // mov ebp, esp
			0x6A, 0xFF, // push -1
		};
		constexpr std::array<UInt8, 5> kFontCreateTextPrologue = {
			0x55,       // push ebp
			0x8B, 0xEC, // mov ebp, esp
			0x6A, 0xFF, // push -1
		};
		constexpr std::array<UInt8, 9> kFontMakeStringPrologue = {
			0x55,                               // push ebp
			0x8B, 0xEC,                         // mov ebp, esp
			0x81, 0xEC, 0xAC, 0x00, 0x00, 0x00, // sub esp, 000000ACh
		};
		constexpr std::array<UInt8, 6> kCalculateDimensionsPrologue = {
			0x55,             // push ebp
			0x8B, 0xEC,       // mov ebp, esp
			0x83, 0xEC, 0x4C, // sub esp, 4Ch
		};
		std::array<hook_site::EntryJumpSite, 5> s_coreFontEntrySites = {{
			{ "Font::Font entry (__thiscall member)", kFontConstructor,
				kFontConstructorPrologue, &FontEx::FontConstructor },
			{ "Font::Load entry (__thiscall member)", kFontLoad,
				kFontLoadPrologue, &FontEx::Load },
			{ "Font::CreateText entry (__thiscall member)", kFontCreateText,
				kFontCreateTextPrologue, &FontEx::CreateText },
			{ "Font::MakeString entry (__thiscall member)", kFontMakeString,
				kFontMakeStringPrologue, &FontEx::MakeString },
			{ "FontManager::CalculateStringDimensions entry (__thiscall member)",
				kFontManagerCalculateStringDimensions,
				kCalculateDimensionsPrologue,
				&FontManagerEx::CalculateStringDimensions },
		}};

		struct PendingTrampoline
		{
			hook_site::EntryJumpSite* site = nullptr;
			void* code = nullptr;
		};
		std::array<PendingTrampoline, 5> s_coreFontTrampolines = {};

		template <size_t N>
		bool MatchInstalledCallSitesUnchecked(
			const std::array<hook_site::RelCallSite, N>& sites)
		{
			for (const hook_site::RelCallSite& site : sites)
			{
				if (!site.IsInstalledUnchecked())
				{
					return false;
				}
			}
			return true;
		}

		bool IsInstalledFontHookGraphCurrentUnchecked()
		{
			if (!s_fontHookInstallState.multibyte
				&& !s_fontHookInstallState.freeType)
			{
				return false;
			}
			if (!FontLoadBoundedReadSizeHook::IsInstalledUnchecked())
				return false;

			for (const PendingTrampoline& trampoline : s_coreFontTrampolines)
			{
				if (!trampoline.code || !trampoline.site
					|| !trampoline.site->IsInstalledUnchecked())
				{
					return false;
				}
			}

			if (!MatchInstalledCallSitesUnchecked(
					kCommonFontCallSites))
			{
				return false;
			}

			if (s_fontHookInstallState.multibyte)
			{
				return s_fontPrepTextEntrySite.IsInstalledUnchecked()
					&& MatchInstalledCallSitesUnchecked(
						kMultibyteFontCallSites);
			}

			return MatchInstalledCallSitesUnchecked(
				kFreeTypeOnlyCallSites);
		}

		bool IsPublishedFontHookGraphCurrent()
		{
			const bool graphCurrent =
				IsInstalledFontHookGraphCurrentUnchecked();
			if (!graphCurrent && !s_fontHookGraphMismatchLogged)
			{
				s_fontHookGraphMismatchLogged = true;
				gLog.FormattedMessage(
					"tnvse_font_hook: installed graph is no longer the verified top-level owner; cached multibyte/freetype capabilities revoked");
			}
			else if (graphCurrent)
			{
				s_fontHookGraphMismatchLogged = false;
			}
			return graphCurrent;
		}

		void ClearCoreFontOriginals()
		{
			s_originalFontConstructor = nullptr;
			s_originalFontLoad = nullptr;
			s_originalFontCreateText = nullptr;
			s_originalFontMakeString = nullptr;
			s_originalCalculateStringDimensions = nullptr;
		}

		template <size_t N>
		void ReleaseTrampolines(std::array<PendingTrampoline, N>& trampolines)
		{
			for (PendingTrampoline& trampoline : trampolines)
			{
				if (trampoline.code)
					VirtualFree(trampoline.code, 0, MEM_RELEASE);
				trampoline.code = nullptr;
			}
		}

		template <size_t N>
		bool RestoreOwnedCoreEntries(
			std::array<PendingTrampoline, N>& trampolines)
		{
			bool restored = true;
			for (PendingTrampoline& trampoline : trampolines)
			{
				const bool entryRestored = trampoline.site
					&& trampoline.site->RollbackOwned();
				if (!entryRestored)
				{
					SIZE_T observedTarget = 0;
					if (trampoline.site)
					{
						hook_identity::ReadRel32Target(
							trampoline.site->entryAddress,
							Rel32Opcode::Jump, observedTarget);
					}
					gLog.FormattedMessage(
						"tnvse_font_hook: core entry rollback retained address=%08X observed=%08X replacement=%08X",
						static_cast<UInt32>(trampoline.site
							? trampoline.site->entryAddress : 0),
						static_cast<UInt32>(observedTarget),
						static_cast<UInt32>(trampoline.site
							? trampoline.site->replacementTarget : 0));
					restored = false;
				}
			}
			return restored;
		}

		bool RollbackCoreFontEntryHooks()
		{
			bool entriesRestored = true;
			if (s_coreFontTrampolines[0].code)
			{
				entriesRestored = RestoreOwnedCoreEntries(
					s_coreFontTrampolines);
				if (entriesRestored)
				{
					ReleaseTrampolines(s_coreFontTrampolines);
					ClearCoreFontOriginals();
				}
			}
			// If any core entry still reaches a retained trampoline, it can still
			// enter retail Font::Load. Keep the bounded read published in that case.
			const bool readSizeRestored = entriesRestored
				? FontLoadBoundedReadSizeHook::Rollback() : false;
			return entriesRestored && readSizeRestored;
		}

		bool BuildTrampoline(PendingTrampoline& trampoline)
		{
			if (!trampoline.site || !trampoline.site->MatchesOriginalBytes())
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: original entry signature mismatch address=%08X length=%u",
					static_cast<UInt32>(trampoline.site
						? trampoline.site->entryAddress : 0),
					static_cast<UInt32>(trampoline.site
						? trampoline.site->originalLength : 0));
				return false;
			}
			hook_site::EntryJumpSite& site = *trampoline.site;

			constexpr SIZE_T kJumpInstructionSize = 5;
			const SIZE_T trampolineSize =
				site.originalLength + kJumpInstructionSize;
			UInt8* code = static_cast<UInt8*>(VirtualAlloc(nullptr,
				trampolineSize, MEM_COMMIT | MEM_RESERVE,
				PAGE_READWRITE));
			if (!code)
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline allocation failed address=%08X",
					static_cast<UInt32>(site.entryAddress));
				return false;
			}

			// Build the complete image privately, then publish it through the same
			// checked SafeWrite backend used for every process-level hook site.
			std::array<UInt8, 32> image = {};
			if (trampolineSize > image.size())
			{
				VirtualFree(code, 0, MEM_RELEASE);
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline image exceeds backend buffer address=%08X size=%u",
					static_cast<UInt32>(site.entryAddress),
					static_cast<UInt32>(trampolineSize));
				return false;
			}
			std::memcpy(image.data(),
				reinterpret_cast<const void*>(site.entryAddress),
				site.originalLength);
			// JMP rel32 back to entry + copied length.
			image[site.originalLength] =
				static_cast<UInt8>(Rel32Opcode::Jump);
			const UInt32 returnDisplacement =
				static_cast<UInt32>(site.entryAddress + site.originalLength
					- reinterpret_cast<SIZE_T>(code + trampolineSize));
			std::memcpy(image.data() + site.originalLength + 1,
				&returnDisplacement, sizeof(returnDisplacement));
			if (!SafeWriteBuf(reinterpret_cast<SIZE_T>(code), image.data(),
				trampolineSize))
			{
				VirtualFree(code, 0, MEM_RELEASE);
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline publication failed address=%08X",
					static_cast<UInt32>(site.entryAddress));
				return false;
			}
			DWORD previousProtect = 0;
			if (!VirtualProtect(code, trampolineSize, PAGE_EXECUTE_READ,
					&previousProtect))
			{
				const DWORD error = GetLastError();
				VirtualFree(code, 0, MEM_RELEASE);
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline executable protection failed address=%08X error=%lu",
					static_cast<UInt32>(site.entryAddress), error);
				return false;
			}
			if (!FlushInstructionCache(GetCurrentProcess(), code, trampolineSize))
			{
				const DWORD error = GetLastError();
				VirtualFree(code, 0, MEM_RELEASE);
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline instruction-cache flush failed address=%08X error=%lu",
					static_cast<UInt32>(site.entryAddress), error);
				return false;
			}
			trampoline.code = code;
			return true;
		}

		bool InstallCoreFontEntryHooks()
		{
			std::array<PendingTrampoline, 5> trampolines = {{
				{ &s_coreFontEntrySites[0] },
				{ &s_coreFontEntrySites[1] },
				{ &s_coreFontEntrySites[2] },
				{ &s_coreFontEntrySites[3] },
				{ &s_coreFontEntrySites[4] },
			}};

			for (PendingTrampoline& trampoline : trampolines)
			{
				if (BuildTrampoline(trampoline))
					continue;
				ReleaseTrampolines(trampolines);
				return false;
			}
			if (!FontLoadBoundedReadSizeHook::Install())
			{
				ReleaseTrampolines(trampolines);
				return false;
			}

			s_originalFontConstructor =
				reinterpret_cast<FontConstructorFn>(trampolines[0].code);
			s_originalFontLoad = reinterpret_cast<FontLoadFn>(trampolines[1].code);
			s_originalFontCreateText = reinterpret_cast<FontCreateTextFn>(trampolines[2].code);
			s_originalFontMakeString = reinterpret_cast<FontMakeStringFn>(trampolines[3].code);
			s_originalCalculateStringDimensions =
				reinterpret_cast<CalculateStringDimensionsFn>(trampolines[4].code);

			WriteRelJumpEx(kFontConstructor, &FontEx::FontConstructor);
			WriteRelJumpEx(kFontLoad, &FontEx::Load);
			WriteRelJumpEx(kFontCreateText, &FontEx::CreateText);
			WriteRelJumpEx(kFontMakeString, &FontEx::MakeString);
			PatchMemoryNop(kFontMakeString + 5,
				kFontMakeStringPrologue.size() - 5);
			WriteRelJumpEx(kFontManagerCalculateStringDimensions,
				&FontManagerEx::CalculateStringDimensions);
			PatchMemoryNop(kFontManagerCalculateStringDimensions + 5,
				kCalculateDimensionsPrologue.size() - 5);

			bool installed = FontLoadBoundedReadSizeHook::IsInstalled();
			for (const hook_site::EntryJumpSite& site : s_coreFontEntrySites)
				installed = site.IsInstalled() && installed;
			if (!installed)
			{
				const bool entriesRestored = RestoreOwnedCoreEntries(
					trampolines);
				const bool readSizeRestored = entriesRestored
					? FontLoadBoundedReadSizeHook::Rollback() : false;
				if (entriesRestored)
				{
					ReleaseTrampolines(trampolines);
					ClearCoreFontOriginals();
				}
				else
				{
					// At least one live entry can still reach these trampolines.
					// Retain their storage and original-call pointers for safety.
					s_coreFontTrampolines = trampolines;
				}
				gLog.FormattedMessage(
					"tnvse_font_hook: core entry write verification failed; rollbackEntries=%s rollbackReadSize=%s trampolines=%s",
					entriesRestored ? "restored" : "incomplete",
					readSizeRestored ? "restored" : "incomplete",
					entriesRestored ? "released" : "retained-for-live-hook-safety");
				return false;
			}
			s_coreFontTrampolines = trampolines;
			gLog.FormattedMessage(
				"tnvse_font_hook: core entries installed Font::CreateText=00A12880 route=caller-independent-thiscall prepared_sidecar=scoped-capture-no-hash");
			return true;
		}

		constexpr SIZE_T kTileTextMakeNodeVTableEntry = 0x1094880;
		constexpr SIZE_T kVanillaTileTextMakeNode = 0xA21AF0;
		using TileTextMakeNodeFn = NiNode* (__thiscall*)(TileText*);
		TileTextMakeNodeFn s_tileTextMakeNode = nullptr;
		thread_local UInt32 s_effectSuppressionDepth = 0;
		thread_local UInt32 s_vuiProxyMeasureOnlyDepth = 0;
		Font* ResolveVuiEffectProxyFont(TileText* tile)
		{
			if (!tile)
				return nullptr;
			UInt32 fontId = 0;
			if (!TryResolveGameFontId(
				tile->GetValueFloat(Tile::kTileValue_font), fontId))
			{
				return nullptr;
			}
			return ResolveGameFont(FontManager::GetSingleton(), fontId);
		}

		bool IsVuiEffectProxy(const TileText* tile)
		{
			// VUI+'s Prefabs/VUI+/outline.xml implements its original-style dark
			// shadow/outline by cloning the source text into these two named tiles.
			// Always let those proxy tiles complete text preparation so their width
			// and height traits remain valid for anonymous sibling expressions.  When
			// tNVSE already supplies the effect, skip their glyph emission and cull
			// only the finished scene node.
			if (!tile)
				return false;
			const char* name = tile->strName.c_str();
			if (!name)
				return false;
			return _stricmp(name, "VUI+Shadow") == 0
				|| _stricmp(name, "VUI+Outline") == 0;
		}

		bool IsPrewarmOverlayText(const TileText* tile)
		{
			// LoadingMenu applies Tile changes during its locked traversal, but
			// TileText::MakeNode is deferred until ShowChanges. Identify only the
			// tNVSE prewarm subtree at that real geometry boundary. FreeType remains
			// active, while native shape creation skips renderer precache until the
			// normal sorted submission path. The bounded walk also fails closed on a
			// corrupt or cyclic parent chain.
			constexpr UInt32 kMaximumParentDepth = 32;
			const Tile* current = tile;
			for (UInt32 depth = 0;
				current && depth < kMaximumParentDepth;
				++depth, current = current->pParent)
			{
				const char* name = current->strName.c_str();
				if (name && _stricmp(name, "tNVSE_Prewarm") == 0)
					return true;
			}
			return false;
		}

		class ScopedEffectSuppression
		{
		public:
			explicit ScopedEffectSuppression(bool suppress) : m_suppress(suppress)
			{
				if (m_suppress)
					++s_effectSuppressionDepth;
			}

			~ScopedEffectSuppression()
			{
				if (m_suppress)
					--s_effectSuppressionDepth;
			}

		private:
			bool m_suppress;
		};

		class ScopedVuiProxyMeasureOnly
		{
		public:
			explicit ScopedVuiProxyMeasureOnly(bool enabled) : m_enabled(enabled)
			{
				if (m_enabled)
					++s_vuiProxyMeasureOnlyDepth;
			}

			~ScopedVuiProxyMeasureOnly()
			{
				if (m_enabled)
					--s_vuiProxyMeasureOnlyDepth;
			}

		private:
			bool m_enabled;
		};

		NiNode* __fastcall TileTextMakeNodeHook(TileText* tile, void*)
		{
			const bool suppress = IsVuiEffectProxy(tile);
			Font* font = suppress ? ResolveVuiEffectProxyFont(tile) : nullptr;
			const bool replaceProxy = suppress && HasEnabledFreeTypeFontEffects(font);
			const bool useNoPrecachePrewarmRoute = IsPrewarmOverlayText(tile);

			ScopedEffectSuppression scope(suppress);
			ScopedVuiProxyMeasureOnly measureOnly(replaceProxy);
			NiNode* node = nullptr;
			if (useNoPrecachePrewarmRoute)
			{
				// This scope must cover the actual deferred MakeNode call. A scope
				// around SetText/RebuildTextGeometry ends before LoadingMenu reaches
				// ShowChanges and therefore cannot protect this boundary.
				ScopedFreeTypeNoPrecacheRoute noPrecacheRoute;
				node = s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;
			}
			else
			{
				node = s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;
			}

			if (replaceProxy && node)
				node->SetAppCulled(true);
			if (node)
				pipboy_search_icon_compat::NormalizeInactiveEmptySearchWidth(tile);
			return node;
		}

		bool InstallVuiEffectProxyCompatibility()
		{
			if (!hook_identity::IsAccessibleRegion(
				kTileTextMakeNodeVTableEntry, sizeof(SIZE_T), false))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode vtable entry is unreadable entry=%08X",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry));
				return false;
			}
			const SIZE_T currentHandler = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			const SIZE_T adapterHandler =
				reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
			if (currentHandler == adapterHandler)
			{
				return s_tileTextMakeNode
					&& reinterpret_cast<SIZE_T>(s_tileTextMakeNode)
						!= adapterHandler
					&& hook_identity::IsExecutableTarget(
						reinterpret_cast<SIZE_T>(s_tileTextMakeNode));
			}
			if (!hook_identity::IsExecutableTarget(currentHandler))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode target is not executable target=%08X",
					static_cast<UInt32>(currentHandler));
				return false;
			}
			const TileTextMakeNodeFn previousMakeNode = s_tileTextMakeNode;
			s_tileTextMakeNode =
				reinterpret_cast<TileTextMakeNodeFn>(currentHandler);
			// TileText::MakeNode vtable slot
			// (__thiscall target via __fastcall shim).
			const SafeWrite32IfEqualResult publication =
				SafeWrite32IfEqualDetailed(kTileTextMakeNodeVTableEntry,
					adapterHandler, currentHandler);
			const bool published = publication.WasPublished();
			if (!published)
			{
				const SIZE_T observedHandler = publication.comparisonPerformed
					? publication.observed
					: *reinterpret_cast<const SIZE_T*>(
						kTileTextMakeNodeVTableEntry);
				s_tileTextMakeNode = previousMakeNode;
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility CAS did not publish entry=%08X predecessor=%08X observed=%08X compared=%u protectionError=%lu",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
					static_cast<UInt32>(currentHandler),
					static_cast<UInt32>(observedHandler),
					publication.comparisonPerformed ? 1u : 0u,
					publication.protectionError);
				return false;
			}
			if (!publication.PostconditionsComplete())
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility published with incomplete write postconditions protectionRestored=%u protectionError=%lu cacheFlushed=%u cacheError=%lu",
					publication.protectionRestored ? 1u : 0u,
					publication.protectionError,
					publication.instructionCacheFlushed ? 1u : 0u,
					publication.cacheFlushError);
			}
			const SIZE_T observedHandler = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			if (observedHandler == adapterHandler)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility installed entry=%08X target=%08X chained=%d",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
					static_cast<UInt32>(currentHandler),
					currentHandler != kVanillaTileTextMakeNode ? 1 : 0);
				return true;
			}

			if (observedHandler == currentHandler)
			{
				s_tileTextMakeNode = previousMakeNode;
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility was published but the slot returned to its predecessor entry=%08X predecessor=%08X",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
					static_cast<UInt32>(currentHandler));
				return false;
			}

			// A later vtable owner may already use this hook as its predecessor.
			// Keep that predecessor alive and do not overwrite the new top slot.
			gLog.FormattedMessage(
				"tnvse_freetype_font: VUI+ effect proxy compatibility may be retained below observed handler=%08X predecessor=%08X executable=%u; reachability unverified",
				static_cast<UInt32>(observedHandler),
				static_cast<UInt32>(currentHandler),
				hook_identity::IsExecutableTarget(observedHandler) ? 1u : 0u);
			return false;
		}

		FontHookInstallState FinalizeFontHookGraph(
			bool multibyte, bool freeType)
		{
			const bool commonInstalled = VerifyInstalledCallSites(
				kCommonFontCallSites);
			bool modeInstalled = false;
			bool prepTextInstalled = true;
			SIZE_T prepTextTarget = 0;

			if (multibyte)
			{
				modeInstalled = VerifyInstalledCallSites(
					kMultibyteFontCallSites);
				prepTextTarget = s_fontPrepTextEntrySite.replacementTarget;
				prepTextInstalled = VerifyFontPrepTextEntry(prepTextTarget);
			}
			else
			{
				modeInstalled = VerifyInstalledCallSites(
					kFreeTypeOnlyCallSites);
			}

			if (!commonInstalled || !modeInstalled || !prepTextInstalled)
			{
				const bool commonRestored = RollbackOwnedCallSites(
					kCommonFontCallSites);
				bool modeRestored = false;
				bool prepTextRestored = true;
				if (multibyte)
				{
					modeRestored = RollbackOwnedCallSites(
						kMultibyteFontCallSites);
					prepTextRestored = RollbackOwnedFontPrepTextEntry(
						prepTextTarget);
				}
				else
				{
					modeRestored = RollbackOwnedCallSites(
						kFreeTypeOnlyCallSites);
				}
				const bool coreRestored = RollbackCoreFontEntryHooks();
				s_fontHookInstallState = {};
				gLog.FormattedMessage(
					"tnvse_font_hook: installation transaction aborted common=%u mode=%u prepText=%u rollbackCommon=%u rollbackMode=%u rollbackPrepText=%u rollbackCore=%u",
					commonInstalled ? 1u : 0u,
					modeInstalled ? 1u : 0u,
					prepTextInstalled ? 1u : 0u,
					commonRestored ? 1u : 0u,
					modeRestored ? 1u : 0u,
					prepTextRestored ? 1u : 0u,
					coreRestored ? 1u : 0u);
				return s_fontHookInstallState;
			}

			s_fontHookInstallState.multibyte = multibyte;
			s_fontHookInstallState.freeType = freeType;
			if (freeType)
				InstallVuiEffectProxyCompatibility();
			return s_fontHookInstallState;
		}

	}

	Font* CallOriginalFontConstructor(
		Font* font, int fontNum, char* filename, bool load)
	{
		return s_originalFontConstructor
			? s_originalFontConstructor(font, fontNum, filename, load) : nullptr;
	}

	void CallOriginalFontLoad(Font* font)
	{
		if (s_originalFontLoad)
			s_originalFontLoad(font);
	}

	void CallOriginalFontCreateText(
		Font* font, BSStringT<char>* text, int* width, int* height,
		int lineStart, int lineEnd, int flags, char lineBreak,
		const NiColorA* color, NiTriShape** textShape, NiTriShape** iconShape)
	{
		if (s_originalFontCreateText)
		{
			s_originalFontCreateText(font, text, width, height, lineStart,
				lineEnd, flags, lineBreak, color, textShape, iconShape);
		}
	}

	NiAVObject* CallOriginalFontMakeString(
		Font* font, float startX, float startY, float z,
		BSStringT<char>* text, int* width, bool prepareObject,
		const NiColorA* color, bool upperLeftCorner, bool prepareObjectFinal)
	{
		return s_originalFontMakeString
			? s_originalFontMakeString(font, startX, startY, z, text, width,
				prepareObject, color, upperLeftCorner, prepareObjectFinal) : nullptr;
	}

	NiPoint3* CallOriginalCalculateStringDimensions(
		FontManager* manager, NiPoint3* dimensions, const char* text,
		UInt32 fontId, float maxWrapWidth, UInt32 startCharIndex)
	{
		return s_originalCalculateStringDimensions
			? s_originalCalculateStringDimensions(manager, dimensions, text,
				fontId, maxWrapWidth, startCharIndex) : dimensions;
	}

	bool AreMultibyteFontHooksInstalled()
	{
		return s_fontHookInstallState.multibyte
			&& IsPublishedFontHookGraphCurrent();
	}

	bool AreFreeTypeFontHooksInstalled()
	{
		return s_fontHookInstallState.freeType
			&& IsPublishedFontHookGraphCurrent();
	}

	bool IsPrewarmOverlayMakeNodeRouteInstalled()
	{
		if (!hook_identity::IsAccessibleRegion(
			kTileTextMakeNodeVTableEntry, sizeof(SIZE_T), false))
		{
			return false;
		}
		const SIZE_T adapterHandler =
			reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
		return *reinterpret_cast<const SIZE_T*>(kTileTextMakeNodeVTableEntry)
				== adapterHandler
			&& s_tileTextMakeNode
			&& reinterpret_cast<SIZE_T>(s_tileTextMakeNode) != adapterHandler
			&& hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(s_tileTextMakeNode));
	}

	bool IsFreeTypeEffectSuppressionActive()
	{
		return s_effectSuppressionDepth != 0;
	}

	bool IsFreeTypeVuiProxyMeasureOnlyActive()
	{
		return s_vuiProxyMeasureOnlyDepth != 0;
	}

	void InitBigGunsDescHooks()
	{
		constexpr SIZE_T kJipInstruction = 0x100113BD;
		constexpr SIZE_T kJipImmediate = kJipInstruction + 1;
		constexpr SIZE_T kJipVanillaDescription = 0x1005D130;
		constexpr UInt8 kMoveEdxImmediate32Opcode = 0xBA; // MOV EDX, imm32
		const SIZE_T instruction = GetJIPAddress(kJipInstruction);
		const SIZE_T immediate = GetJIPAddress(kJipImmediate);
		const UInt32 vanillaDescription = static_cast<UInt32>(
			GetJIPAddress(kJipVanillaDescription));
		if (!hook_identity::IsAccessibleRegion(instruction, 5, true)
			|| *reinterpret_cast<const UInt8*>(instruction)
				!= kMoveEdxImmediate32Opcode
			|| *reinterpret_cast<const UInt32*>(immediate)
				!= vanillaDescription)
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: JIP Big Guns description signature mismatch instruction=%08X; code left untouched",
				static_cast<UInt32>(instruction));
			return;
		}

		static std::string sConvertedBigGunsDesc = IsEastAsianUiMode()
			? UTF8ToMultiByteStr(g_sNewBigGunsDesc, g_usingWinEncoding)
			: g_sNewBigGunsDesc;
		const UInt32 replacement = reinterpret_cast<UInt32>(
			sConvertedBigGunsDesc.c_str());
		// JIP Big Guns description: MOV EDX, imm32 data operand.
		SafeWrite32(immediate, replacement);
		const UInt32 observed = *reinterpret_cast<const UInt32*>(immediate);
		if (observed != replacement)
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: JIP Big Guns description write verification failed observed=%08X rollback=%s",
				observed,
				observed == vanillaDescription
					? "vanilla-remains" : "later-owner-retained");
		}
	}

	static bool InstallDoorPromptHook(SIZE_T formatterAdapterTarget,
		const char* mode)
	{
		hook_site::RelCallSite doorPromptCallSite{
			"Door prompt -> BSsprintf (__cdecl)",
			0x777006,
			0x406D00,
			formatterAdapterTarget
		};
		if (!doorPromptCallSite.IsExpected())
			return false;
		WriteRelCall(0x777006, formatterAdapterTarget);
		if (doorPromptCallSite.IsInstalled())
		{
			return true;
		}
		SIZE_T observedTarget = 0;
		const bool readable = doorPromptCallSite.ReadTarget(observedTarget);
		gLog.FormattedMessage(
			"tnvse_font_hook: door prompt %s adapter write verification failed observed=%08X readable=%u rollback=%s",
			mode,
			static_cast<UInt32>(observedTarget),
			readable ? 1u : 0u,
			readable && observedTarget == doorPromptCallSite.expectedTarget
				? "vanilla-remains" : "later-owner-or-invalid-retained");
		return false;
	}

	void InitDoorPromptHooksCHS()
	{
		InstallDoorPromptHook(
			reinterpret_cast<SIZE_T>(&BSsprintfHookCHS), "CHS");
	}

	void InitDoorPromptHooksKOR()
	{
		InstallDoorPromptHook(
			reinterpret_cast<SIZE_T>(&BSsprintfHookKOR), "KOR");
	}

	void InitPluralHooks()
	{
		constexpr SIZE_T kPluralBranch = 0x753E39;
		constexpr UInt8 kJumpIfEqualShortOpcode = 0x74; // JE rel8
		constexpr UInt8 kJumpShortOpcode = 0xEB;        // JMP rel8
		constexpr std::array<UInt8, 1> kExpectedJumpIfEqual = {
			kJumpIfEqualShortOpcode,
		};
		constexpr std::array<UInt8, 1> kPatchedUnconditionalJump = {
			kJumpShortOpcode,
		};
		hook_site::BytePatchSite pluralBranchPatchSite{
			"Terminal plural conditional JE -> JMP",
			kPluralBranch,
			kExpectedJumpIfEqual,
			kPatchedUnconditionalJump
		};
		if (!pluralBranchPatchSite.MatchesOriginalBytes())
		{
			const UInt32 actual = hook_identity::IsAccessibleRegion(
				kPluralBranch, sizeof(UInt8), true)
				? *reinterpret_cast<const UInt8*>(kPluralBranch)
				: 0xFFFFFFFFu;
			gLog.FormattedMessage(
				"tnvse_font_hook: identity mismatch site=plural branch address=00753E39 expected=74 actual=%08X",
				actual);
			return;
		}
		SafeWrite8(kPluralBranch, kJumpShortOpcode);
		const UInt8 observed = *reinterpret_cast<const UInt8*>(kPluralBranch);
		if (observed != kJumpShortOpcode)
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: plural branch write verification failed observed=%02X rollback=%s",
				static_cast<UInt32>(observed),
				observed == kJumpIfEqualShortOpcode
					? "vanilla-remains" : "later-owner-retained");
		}
	}

	FontHookInstallState InitFontHooks()
	{
		s_fontHookInstallState = {};
		s_fontHookGraphMismatchLogged = false;
		if (!g_bEnableMultibyteFontHook && !g_bEnableFreeTypeFontRendering)
		{
			gLog.FormattedMessage("tnvse_font_hook: all font hooks disabled by tnvse.ini");
			return s_fontHookInstallState;
		}
		if (!ValidateRequiredFontHookSites())
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: installation aborted before patching because the retail hook graph does not match FalloutNV.exe 1.4.0.525");
			return s_fontHookInstallState;
		}
		if (!InstallCoreFontEntryHooks())
		{
			if (g_bEnableMultibyteFontHook)
				gLog.FormattedMessage(
					"tnvse_font_hook: multibyte capability unavailable because core entry validation failed");
			if (g_bEnableFreeTypeFontRendering)
				gLog.FormattedMessage(
					"tnvse_font_hook: freetype capability unavailable because core entry validation failed");
			return s_fontHookInstallState;
		}

		// FontManager::CreateText -> FontManager::PrepText (__thiscall member)
		WriteRelCallEx(0xA18F4A, &FontManagerEx::PrepText);
		// FontManager::CreateText -> TextDoc::Render (__thiscall member)
		WriteRelCallEx(0xA18F63, &FontManagerEx::TextDocRender);
		// TextDoc::Render -> Font::AddChar (__thiscall member)
		WriteRelCallEx(0xA19622, &FontEx::TextDocRenderAddChar);
		// Terminal text -> Font::PrepText (__thiscall member)
		WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);
		// TextLine wrap -> TextLine::AddChar (__thiscall member)
		WriteRelCallEx(0xA19C80, &FontManagerEx::TextLineAddChar);

		if (!g_bEnableMultibyteFontHook)
		{
			// TextLine constructor -> TextLine::AddChar (__thiscall member)
			WriteRelCallEx(0xA1BDE2, &FontManagerEx::TextLineAddChar);
			FinalizeFontHookGraph(false, g_bEnableFreeTypeFontRendering);
			if (!s_fontHookInstallState.freeType)
				return s_fontHookInstallState;
			gLog.FormattedMessage(
				"tnvse_font_hook: installed mode=freetype-custom-single-byte configuredCodePage=%u freeTypeCodePage=%u",
				g_usingWinEncoding, GetFreeTypeTextCodePage());
			return s_fontHookInstallState;
		}

		// Font::PrepText entry -> FontEx::PrepText (__thiscall member)
		WriteRelJumpEx(kFontPrepText, &FontEx::PrepText);

		// AnimatingText::Update -> encoded-unit memcpy (__cdecl)
		WriteRelCall(0x6FFFEE, &CopyAnimatingTextEncodedUnits);
		// FontManager::PrepText -> PrepHypertext (__thiscall member)
		WriteRelCallEx(0xA18ACC, &FontManagerEx::PrepHypertext);

		// FontManager::PrepHypertext -> CollectTo
		// (static __fastcall thiscall shims)
		WriteRelCall(0xA1772D, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17835, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17A1E, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17B65, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17BB1, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17CFE, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17D5D, &FontManagerEx::CollectToAttributeValue);
		WriteRelCall(0xA17DE9, &FontManagerEx::CollectToAttributeValue);

		// FontManager::CreateText -> TextDoc::~TextDoc (__thiscall member)
		WriteRelCallEx(0xA18F7D, &FontManagerEx::TextDocDestructor);

		// PrepHypertext / PrepText -> TextDoc::AddChar (__thiscall member)
		WriteRelCallEx(0xA178A4, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA179D9, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA17FC2, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA18D7C, &FontManagerEx::TextDocAddChar);

		// TextDoc / TextPage -> TextPage::AddChar (__thiscall member)
		WriteRelCallEx(0xA19A6F, &FontManagerEx::TextPageAddChar);
		WriteRelCallEx(0xA1BD1C, &FontManagerEx::TextPageAddChar);

		// PrepHypertext / PrepText -> CharData::Copy
		// (static __fastcall thiscall shims)
		WriteRelCall(0xA17898, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA179CD, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA17FB6, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA18D73, &FontManagerEx::CharDataCopy);

		// Quest / location Tile::SetString adapters (__fastcall)
		WriteRelCall(0x77AF4B, &TileSetStringHookForQuestAndLocationText);
		WriteRelCall(0x772B5E, &TileSetStringHookForQuestAndLocationText);

		// Terminal / location string conversion adapters (__fastcall)
		WriteRelCall(0x7591AC, &BSString_c_strHook);
		WriteRelCall(0x772B4B, &BSString_GetCStringOrEmptyHook);

		// Quest text strcpy_s adapters (__cdecl)
		WriteRelCall(0x77ACCC, &strcpy_sHook);
		WriteRelCall(0x77ACF8, &strcpy_sHook);

		FinalizeFontHookGraph(true, g_bEnableFreeTypeFontRendering);
		if (!s_fontHookInstallState.multibyte)
			return s_fontHookInstallState;
		gLog.FormattedMessage(
			"tnvse_font_hook: installed mode=%s configuredCodePage=%u freeTypeCodePage=%u",
			s_fontHookInstallState.freeType ? "multibyte-freetype" : "multibyte-original",
			g_usingWinEncoding, GetFreeTypeTextCodePage());
		return s_fontHookInstallState;
	}

} // namespace fonthook
