#include "font_engine.h"
#include "font_manager.h"
#include "font_vector.h"
#include "font_vector_internal.h"
#include "game_hooks.h"
#include "hook_identity.h"
#include "load_config.h"
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
		using hook_identity::Rel32Site;

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
			inline static constexpr std::array<UInt8, 5> kVanillaInstructions = {
				0x8B, 0x50, 0x28, 0xFF, 0xD2
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

			static bool HasVanillaInstructions()
			{
				return hook_identity::IsAccessibleRegion(
						kCallSite, kVanillaInstructions.size(), true)
					&& std::memcmp(reinterpret_cast<const void*>(kCallSite),
						kVanillaInstructions.data(),
						kVanillaInstructions.size()) == 0;
			}

			static bool IsInstalled()
			{
				return hook_identity::MatchesRel32Target(
					kCallSite, Rel32Opcode::Call, Target());
			}

			static bool IsInstalledUnchecked()
			{
				return hook_identity::MatchesRel32TargetUnchecked(
					kCallSite, Rel32Opcode::Call, Target());
			}

			static bool ValidateVanilla()
			{
				if (HasVanillaInstructions())
					return true;
				gLog.FormattedMessage(
					"tnvse_font_hook: identity mismatch site=Font::Load BSFile::GetSize address=%08X expected=8B5028FFD2",
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

				SIZE_T observed = 0;
				const bool readable = hook_identity::ReadRel32Target(
					kCallSite, Rel32Opcode::Call, observed);
				const bool restored = Rollback();
				gLog.FormattedMessage(
					"tnvse_font_hook: Font::Load bounded FNT read write verification failed site=%08X expected=%08X observed=%08X readable=%u rollback=%s",
					static_cast<UInt32>(kCallSite),
					static_cast<UInt32>(Target()),
					static_cast<UInt32>(observed),
					readable ? 1u : 0u,
					restored ? "restored" : "incomplete");
				return false;
			}

			static bool Rollback()
			{
				if (IsInstalled())
				{
					SafeWriteBuf(kCallSite, kVanillaInstructions.data(),
						kVanillaInstructions.size());
				}
				if (HasVanillaInstructions())
					return true;

				SIZE_T observed = 0;
				const bool readable = hook_identity::ReadRel32Target(
					kCallSite, Rel32Opcode::Call, observed);
				gLog.FormattedMessage(
					"tnvse_font_hook: Font::Load bounded FNT read rollback retained site=%08X observed=%08X hook=%08X readable=%u",
					static_cast<UInt32>(kCallSite),
					static_cast<UInt32>(observed),
					static_cast<UInt32>(Target()),
					readable ? 1u : 0u);
				return false;
			}
		};

		inline constexpr Rel32Site kDoorPromptCallSite = {
			0x777006, 0x406D00, "Door prompt -> BSsprintf"
		};

		constexpr std::array<Rel32Site, 5> kCommonFontCallSites = {{
			{ 0xA18F4A, 0xA18A30, "FontManager::CreateText -> FontManager::PrepText" },
			{ 0xA18F63, 0xA19060, "FontManager::CreateText -> TextDoc::Render" },
			{ 0xA19622, 0xA142D0, "TextDoc::Render -> Font::AddChar" },
			{ 0x759281, 0xA12FB0, "Terminal text -> Font::PrepText" },
			{ 0xA19C80, 0xA19F70, "TextLine wrap -> TextLine::AddChar" },
		}};

		constexpr std::array<Rel32Site, 1> kFreeTypeOnlyCallSites = {{
			{ 0xA1BDE2, 0xA19F70, "TextLine constructor -> TextLine::AddChar" },
		}};

		constexpr std::array<Rel32Site, 27> kMultibyteFontCallSites = {{
			{ 0x6FFFEE, 0x401460, "AnimatingText::Update -> memcpy" },
			{ 0xA18ACC, 0xA17390, "FontManager::PrepText -> PrepHypertext" },
			{ 0xA1772D, 0xA16EA0, "PrepHypertext CollectTo[0]" },
			{ 0xA17835, 0xA16EA0, "PrepHypertext CollectTo[1]" },
			{ 0xA17A1E, 0xA16EA0, "PrepHypertext CollectTo[2]" },
			{ 0xA17B65, 0xA16EA0, "PrepHypertext CollectTo[3]" },
			{ 0xA17BB1, 0xA16EA0, "PrepHypertext CollectTo[4]" },
			{ 0xA17CFE, 0xA16EA0, "PrepHypertext CollectTo[5]" },
			{ 0xA17D5D, 0xA16EA0, "PrepHypertext CollectTo attribute[0]" },
			{ 0xA17DE9, 0xA16EA0, "PrepHypertext CollectTo attribute[1]" },
			{ 0xA18F7D, 0xA1B990, "FontManager::CreateText -> TextDoc::~TextDoc" },
			{ 0xA178A4, 0xA19A10, "PrepHypertext TextDoc::AddChar[0]" },
			{ 0xA179D9, 0xA19A10, "PrepHypertext TextDoc::AddChar[1]" },
			{ 0xA17FC2, 0xA19A10, "PrepHypertext TextDoc::AddChar[2]" },
			{ 0xA18D7C, 0xA19A10, "PrepText TextDoc::AddChar" },
			{ 0xA19A6F, 0xA19C00, "TextDoc::AddChar -> TextPage::AddChar" },
			{ 0xA1BD1C, 0xA19C00, "TextPage constructor -> TextPage::AddChar" },
			{ 0xA17898, 0xA1B660, "PrepHypertext CharData::Copy[0]" },
			{ 0xA179CD, 0xA1B660, "PrepHypertext CharData::Copy[1]" },
			{ 0xA17FB6, 0xA1B660, "PrepHypertext CharData::Copy[2]" },
			{ 0xA18D73, 0xA1B660, "PrepText CharData::Copy" },
			{ 0x77AF4B, 0xA01350, "Quest text -> Tile::SetString" },
			{ 0x772B5E, 0xA01350, "Location text -> Tile::SetString" },
			{ 0x7591AC, 0x559450, "Terminal text -> BSStringT<char>::c_str" },
			{ 0x772B4B, 0x438EB0, "Location text -> BSStringT<char>::GetCStringOrEmpty" },
			{ 0x77ACCC, 0x406D30, "Quest text -> strcpy_s[0]" },
			{ 0x77ACF8, 0x406D30, "Quest text -> strcpy_s[1]" },
		}};

		constexpr std::array<UInt8, 8> kFontPrepTextPrologue = {
			0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xE0, 0x07, 0x00
		};

		template <size_t N>
		bool ValidateVanillaCallSites(
			const std::array<Rel32Site, N>& sites)
		{
			bool valid = true;
			for (const Rel32Site& site : sites)
			{
				SIZE_T actualTarget = 0;
				if (!hook_identity::ReadRel32Target(
					site.address, Rel32Opcode::Call, actualTarget))
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expected=CALL rel32",
						site.name, static_cast<UInt32>(site.address));
					valid = false;
					continue;
				}
				if (actualTarget != site.vanillaTarget)
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expectedTarget=%08X actualTarget=%08X",
						site.name, static_cast<UInt32>(site.address),
						static_cast<UInt32>(site.vanillaTarget),
						static_cast<UInt32>(actualTarget));
					valid = false;
				}
			}
			return valid;
		}

		template <size_t N>
		bool VerifyInstalledCallSites(
			const std::array<Rel32Site, N>& sites,
			const std::array<SIZE_T, N>& hookTargets)
		{
			bool installed = true;
			for (size_t i = 0; i < sites.size(); ++i)
			{
				SIZE_T observed = 0;
				const bool readable = hook_identity::ReadRel32Target(
					sites[i].address, Rel32Opcode::Call, observed);
				if (readable && observed == hookTargets[i])
					continue;
				gLog.FormattedMessage(
					"tnvse_font_hook: installed call verification failed site=%s address=%08X expected=%08X observed=%08X readable=%u",
					sites[i].name,
					static_cast<UInt32>(sites[i].address),
					static_cast<UInt32>(hookTargets[i]),
					static_cast<UInt32>(observed),
					readable ? 1u : 0u);
				installed = false;
			}
			return installed;
		}

		template <size_t N>
		bool RollbackOwnedCallSites(
			const std::array<Rel32Site, N>& sites,
			const std::array<SIZE_T, N>& hookTargets)
		{
			bool restored = true;
			for (size_t i = 0; i < sites.size(); ++i)
			{
				SIZE_T observed = 0;
				if (!hook_identity::ReadRel32Target(
						sites[i].address, Rel32Opcode::Call, observed))
				{
					restored = false;
					continue;
				}
				if (observed == hookTargets[i])
				{
					WriteRelCall(sites[i].address, sites[i].vanillaTarget);
					if (!hook_identity::ReadRel32Target(
							sites[i].address, Rel32Opcode::Call, observed))
					{
						restored = false;
						continue;
					}
				}
				if (observed == sites[i].vanillaTarget)
					continue;

				// Another top-level CALL may already retain tNVSE as a
				// predecessor. Never replace that later owner with vanilla.
				gLog.FormattedMessage(
					"tnvse_font_hook: call rollback retained site=%s address=%08X observed=%08X hook=%08X",
					sites[i].name,
					static_cast<UInt32>(sites[i].address),
					static_cast<UInt32>(observed),
					static_cast<UInt32>(hookTargets[i]));
				restored = false;
			}
			return restored;
		}

		bool VerifyFontPrepTextEntry(SIZE_T hookTarget)
		{
			if (hook_identity::MatchesRel32Target(
					kFontPrepText, Rel32Opcode::Jump, hookTarget))
			{
				return true;
			}
			SIZE_T observed = 0;
			const bool readable = hook_identity::ReadRel32Target(
				kFontPrepText, Rel32Opcode::Jump, observed);
			gLog.FormattedMessage(
				"tnvse_font_hook: Font::PrepText entry verification failed expected=%08X observed=%08X readable=%u",
				static_cast<UInt32>(hookTarget),
				static_cast<UInt32>(observed),
				readable ? 1u : 0u);
			return false;
		}

		bool RollbackOwnedFontPrepTextEntry(SIZE_T hookTarget)
		{
			if (hook_identity::MatchesRel32Target(
					kFontPrepText, Rel32Opcode::Jump, hookTarget))
			{
				SafeWriteBuf(kFontPrepText, kFontPrepTextPrologue.data(),
					kFontPrepTextPrologue.size());
			}
			if (hook_identity::IsAccessibleRegion(
					kFontPrepText, kFontPrepTextPrologue.size(), true)
				&& std::memcmp(reinterpret_cast<const void*>(kFontPrepText),
					kFontPrepTextPrologue.data(),
					kFontPrepTextPrologue.size()) == 0)
			{
				return true;
			}

			SIZE_T observed = 0;
			hook_identity::ReadRel32Target(
				kFontPrepText, Rel32Opcode::Jump, observed);
			gLog.FormattedMessage(
				"tnvse_font_hook: Font::PrepText rollback retained observed=%08X hook=%08X",
				static_cast<UInt32>(observed),
				static_cast<UInt32>(hookTarget));
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
				if (!hook_identity::IsAccessibleRegion(
					kFontPrepText, kFontPrepTextPrologue.size(), true)
					|| std::memcmp(reinterpret_cast<const void*>(kFontPrepText),
						kFontPrepTextPrologue.data(),
						kFontPrepTextPrologue.size()) != 0)
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=Font::PrepText address=%08X length=%u",
						static_cast<UInt32>(kFontPrepText),
						static_cast<UInt32>(kFontPrepTextPrologue.size()));
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
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 5> kFontLoadPrologue = {
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 5> kFontCreateTextPrologue = {
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 9> kFontMakeStringPrologue = {
			0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xAC, 0x00, 0x00, 0x00
		};
		constexpr std::array<UInt8, 6> kCalculateDimensionsPrologue = {
			0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C
		};

		struct PendingTrampoline
		{
			SIZE_T source = 0;
			const UInt8* expected = nullptr;
			SIZE_T length = 0;
			void* code = nullptr;
		};
		std::array<PendingTrampoline, 5> s_coreFontTrampolines = {};

		template <class C, class Ret, class... Args>
		SIZE_T MemberFunctionAddress(Ret(C::*target)(Args...))
		{
			static_assert(sizeof(target) == sizeof(SIZE_T),
				"retail x86 hooks require a single-inheritance member pointer");
			union
			{
				Ret(C::*member)(Args...);
				SIZE_T address;
			} conversion = {};
			conversion.member = target;
			return conversion.address;
		}

		bool HasNopTail(SIZE_T source, SIZE_T patchedLength)
		{
			for (SIZE_T offset = 5; offset < patchedLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(source + offset) != 0x90)
					return false;
			}
			return true;
		}

		std::array<SIZE_T, 5> CoreFontHookTargets()
		{
			return {{
				MemberFunctionAddress(&FontEx::FontConstructor),
				MemberFunctionAddress(&FontEx::Load),
				MemberFunctionAddress(&FontEx::CreateText),
				MemberFunctionAddress(&FontEx::MakeString),
				MemberFunctionAddress(
					&FontManagerEx::CalculateStringDimensions),
			}};
		}

		std::array<SIZE_T, kCommonFontCallSites.size()>
		CommonFontCallTargets()
		{
			return {{
				MemberFunctionAddress(&FontManagerEx::PrepText),
				MemberFunctionAddress(&FontManagerEx::TextDocRender),
				MemberFunctionAddress(&FontEx::TextDocRenderAddChar),
				MemberFunctionAddress(&FontEx::PrepTextForTerminal),
				MemberFunctionAddress(&FontManagerEx::TextLineAddChar),
			}};
		}

		std::array<SIZE_T, kFreeTypeOnlyCallSites.size()>
		FreeTypeOnlyCallTargets()
		{
			return {{
				MemberFunctionAddress(&FontManagerEx::TextLineAddChar),
			}};
		}

		std::array<SIZE_T, kMultibyteFontCallSites.size()>
		MultibyteFontCallTargets()
		{
			const SIZE_T collectTo = reinterpret_cast<SIZE_T>(
				&FontManagerEx::CollectTo);
			const SIZE_T collectAttribute = reinterpret_cast<SIZE_T>(
				&FontManagerEx::CollectToAttributeValue);
			const SIZE_T textDocAddChar = MemberFunctionAddress(
				&FontManagerEx::TextDocAddChar);
			const SIZE_T textPageAddChar = MemberFunctionAddress(
				&FontManagerEx::TextPageAddChar);
			const SIZE_T charDataCopy = reinterpret_cast<SIZE_T>(
				&FontManagerEx::CharDataCopy);
			const SIZE_T tileSetString = reinterpret_cast<SIZE_T>(
				&TileSetStringHookForQuestAndLocationText);
			const SIZE_T copyString = reinterpret_cast<SIZE_T>(&strcpy_sHook);
			return {{
				reinterpret_cast<SIZE_T>(&CopyAnimatingTextEncodedUnits),
				MemberFunctionAddress(&FontManagerEx::PrepHypertext),
				collectTo,
				collectTo,
				collectTo,
				collectTo,
				collectTo,
				collectTo,
				collectAttribute,
				collectAttribute,
				MemberFunctionAddress(&FontManagerEx::TextDocDestructor),
				textDocAddChar,
				textDocAddChar,
				textDocAddChar,
				textDocAddChar,
				textPageAddChar,
				textPageAddChar,
				charDataCopy,
				charDataCopy,
				charDataCopy,
				charDataCopy,
				tileSetString,
				tileSetString,
				reinterpret_cast<SIZE_T>(&BSString_c_strHook),
				reinterpret_cast<SIZE_T>(&BSString_GetCStringOrEmptyHook),
				copyString,
				copyString,
			}};
		}

		template <size_t N>
		bool MatchInstalledCallSitesUnchecked(
			const std::array<Rel32Site, N>& sites,
			const std::array<SIZE_T, N>& hookTargets)
		{
			for (size_t i = 0; i < sites.size(); ++i)
			{
				if (!hook_identity::MatchesRel32TargetUnchecked(
					sites[i].address, Rel32Opcode::Call, hookTargets[i]))
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

			const auto coreTargets = CoreFontHookTargets();
			for (size_t i = 0; i < s_coreFontTrampolines.size(); ++i)
			{
				const PendingTrampoline& trampoline =
					s_coreFontTrampolines[i];
				if (!trampoline.code
					|| !hook_identity::MatchesRel32TargetUnchecked(
						trampoline.source, Rel32Opcode::Jump,
						coreTargets[i])
					|| (trampoline.length > 5
						&& !HasNopTail(trampoline.source,
							trampoline.length)))
				{
					return false;
				}
			}

			if (!MatchInstalledCallSitesUnchecked(
					kCommonFontCallSites, CommonFontCallTargets()))
			{
				return false;
			}

			if (s_fontHookInstallState.multibyte)
			{
				return hook_identity::MatchesRel32TargetUnchecked(
						kFontPrepText, Rel32Opcode::Jump,
						MemberFunctionAddress(&FontEx::PrepText))
					&& MatchInstalledCallSitesUnchecked(
						kMultibyteFontCallSites,
						MultibyteFontCallTargets());
			}

			return MatchInstalledCallSitesUnchecked(
				kFreeTypeOnlyCallSites, FreeTypeOnlyCallTargets());
		}

		bool IsPublishedFontHookGraphCurrent()
		{
			const bool current = IsInstalledFontHookGraphCurrentUnchecked();
			if (!current && !s_fontHookGraphMismatchLogged)
			{
				s_fontHookGraphMismatchLogged = true;
				gLog.FormattedMessage(
					"tnvse_font_hook: installed graph is no longer the verified top-level owner; cached multibyte/freetype capabilities revoked");
			}
			else if (current)
			{
				s_fontHookGraphMismatchLogged = false;
			}
			return current;
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
			std::array<PendingTrampoline, N>& trampolines,
			const std::array<SIZE_T, N>& hookTargets)
		{
			bool restored = true;
			for (size_t i = 0; i < trampolines.size(); ++i)
			{
				PendingTrampoline& trampoline = trampolines[i];
				if (hook_identity::MatchesRel32Target(
						trampoline.source, Rel32Opcode::Jump, hookTargets[i]))
				{
					SafeWriteBuf(trampoline.source,
						trampoline.expected, trampoline.length);
				}

				const bool entryRestored = hook_identity::IsAccessibleRegion(
						trampoline.source, trampoline.length, true)
					&& std::memcmp(
						reinterpret_cast<const void*>(trampoline.source),
						trampoline.expected, trampoline.length) == 0;
				if (!entryRestored)
				{
					SIZE_T observed = 0;
					hook_identity::ReadRel32Target(
						trampoline.source, Rel32Opcode::Jump, observed);
					gLog.FormattedMessage(
						"tnvse_font_hook: core entry rollback retained address=%08X observed=%08X hook=%08X",
						static_cast<UInt32>(trampoline.source),
						static_cast<UInt32>(observed),
						static_cast<UInt32>(hookTargets[i]));
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
				const auto hookTargets = CoreFontHookTargets();
				entriesRestored = RestoreOwnedCoreEntries(
					s_coreFontTrampolines, hookTargets);
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
			if (!trampoline.source || !trampoline.expected || trampoline.length < 5
				|| !hook_identity::IsAccessibleRegion(
					trampoline.source, trampoline.length, true)
				|| std::memcmp(reinterpret_cast<const void*>(trampoline.source),
					trampoline.expected, trampoline.length) != 0)
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: original entry signature mismatch address=%08X length=%u",
					static_cast<UInt32>(trampoline.source),
					static_cast<UInt32>(trampoline.length));
				return false;
			}

			constexpr SIZE_T kJumpInstructionSize = 5;
			const SIZE_T trampolineSize =
				trampoline.length + kJumpInstructionSize;
			UInt8* code = static_cast<UInt8*>(VirtualAlloc(nullptr,
				trampolineSize, MEM_COMMIT | MEM_RESERVE,
				PAGE_EXECUTE_READWRITE));
			if (!code)
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline allocation failed address=%08X",
					static_cast<UInt32>(trampoline.source));
				return false;
			}

			// Initialize the complete executable image before publishing or flushing
			// it; this also makes the length+5 boundary explicit to static analysis.
			std::memset(code, 0, trampolineSize);
			std::memcpy(code, reinterpret_cast<const void*>(trampoline.source),
				trampoline.length);
			code[trampoline.length] = 0xE9;
			const UInt32 returnDisplacement =
				static_cast<UInt32>(trampoline.source + trampoline.length
					- reinterpret_cast<SIZE_T>(code + trampolineSize));
			std::memcpy(code + trampoline.length + 1,
				&returnDisplacement, sizeof(returnDisplacement));
			// VirtualAlloc committed trampolineSize bytes and the memset above
			// initialized that entire range. MSVC analysis otherwise retains only
			// the subsequent source-copy length when checking this Win32 API call.
#pragma warning(suppress : 6385)
			FlushInstructionCache(GetCurrentProcess(), code, trampolineSize);
			trampoline.code = code;
			return true;
		}

		bool InstallCoreFontEntryHooks()
		{
			std::array<PendingTrampoline, 5> trampolines = {{
				{ kFontConstructor, kFontConstructorPrologue.data(), kFontConstructorPrologue.size() },
				{ kFontLoad, kFontLoadPrologue.data(), kFontLoadPrologue.size() },
				{ kFontCreateText, kFontCreateTextPrologue.data(), kFontCreateTextPrologue.size() },
				{ kFontMakeString, kFontMakeStringPrologue.data(), kFontMakeStringPrologue.size() },
				{ kFontManagerCalculateStringDimensions,
					kCalculateDimensionsPrologue.data(),
					kCalculateDimensionsPrologue.size() }
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
			PatchMemoryNop(kFontMakeString + 5, kFontMakeStringPrologue.size() - 5);
			WriteRelJumpEx(kFontManagerCalculateStringDimensions,
				&FontManagerEx::CalculateStringDimensions);
			PatchMemoryNop(kFontManagerCalculateStringDimensions + 5,
				kCalculateDimensionsPrologue.size() - 5);

			const auto hookTargets = CoreFontHookTargets();
			bool installed = FontLoadBoundedReadSizeHook::IsInstalled();
			for (size_t i = 0; i < trampolines.size(); ++i)
			{
				installed = hook_identity::MatchesRel32Target(
					trampolines[i].source,
					Rel32Opcode::Jump,
					hookTargets[i]) && installed;
				if (trampolines[i].length > 5)
				{
					installed = HasNopTail(
						trampolines[i].source,
						trampolines[i].length) && installed;
				}
			}
			if (!installed)
			{
				const bool entriesRestored = RestoreOwnedCoreEntries(
					trampolines, hookTargets);
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

			ScopedEffectSuppression scope(suppress);
			ScopedVuiProxyMeasureOnly measureOnly(replaceProxy);
			NiNode* node = s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;

			if (replaceProxy && node)
				node->SetAppCulled(true);
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
			const SIZE_T current = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
			if (current == hook)
			{
				return s_tileTextMakeNode
					&& reinterpret_cast<SIZE_T>(s_tileTextMakeNode) != hook
					&& hook_identity::IsExecutableTarget(
						reinterpret_cast<SIZE_T>(s_tileTextMakeNode));
			}
			if (!hook_identity::IsExecutableTarget(current))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode target is not executable target=%08X",
					static_cast<UInt32>(current));
				return false;
			}
			s_tileTextMakeNode = reinterpret_cast<TileTextMakeNodeFn>(current);
			SafeWrite32(kTileTextMakeNodeVTableEntry, hook);
			const SIZE_T observed = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			if (observed == hook)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility installed entry=%08X target=%08X chained=%d",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
					static_cast<UInt32>(current),
					current != kVanillaTileTextMakeNode ? 1 : 0);
				return true;
			}

			if (observed == current)
			{
				s_tileTextMakeNode = nullptr;
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility write did not publish entry=%08X predecessor=%08X",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
					static_cast<UInt32>(current));
				return false;
			}

			// A later vtable owner may already use this hook as its predecessor.
			// Keep that predecessor alive and do not overwrite the new top slot.
			gLog.FormattedMessage(
				"tnvse_freetype_font: VUI+ effect proxy compatibility may be retained below observed handler=%08X predecessor=%08X executable=%u; reachability unverified",
				static_cast<UInt32>(observed),
				static_cast<UInt32>(current),
				hook_identity::IsExecutableTarget(observed) ? 1u : 0u);
			return false;
		}

		FontHookInstallState FinalizeFontHookGraph(
			bool multibyte, bool freeType)
		{
			const auto commonTargets = CommonFontCallTargets();
			const bool commonInstalled = VerifyInstalledCallSites(
				kCommonFontCallSites, commonTargets);
			bool modeInstalled = false;
			bool prepTextInstalled = true;
			SIZE_T prepTextTarget = 0;

			if (multibyte)
			{
				const auto multibyteTargets = MultibyteFontCallTargets();
				modeInstalled = VerifyInstalledCallSites(
					kMultibyteFontCallSites, multibyteTargets);
				prepTextTarget = MemberFunctionAddress(&FontEx::PrepText);
				prepTextInstalled = VerifyFontPrepTextEntry(prepTextTarget);
			}
			else
			{
				const auto freeTypeTargets = FreeTypeOnlyCallTargets();
				modeInstalled = VerifyInstalledCallSites(
					kFreeTypeOnlyCallSites, freeTypeTargets);
			}

			if (!commonInstalled || !modeInstalled || !prepTextInstalled)
			{
				const bool commonRestored = RollbackOwnedCallSites(
					kCommonFontCallSites, commonTargets);
				bool modeRestored = false;
				bool prepTextRestored = true;
				if (multibyte)
				{
					const auto multibyteTargets = MultibyteFontCallTargets();
					modeRestored = RollbackOwnedCallSites(
						kMultibyteFontCallSites, multibyteTargets);
					prepTextRestored = RollbackOwnedFontPrepTextEntry(
						prepTextTarget);
				}
				else
				{
					const auto freeTypeTargets = FreeTypeOnlyCallTargets();
					modeRestored = RollbackOwnedCallSites(
						kFreeTypeOnlyCallSites, freeTypeTargets);
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
		const SIZE_T instruction = GetJIPAddress(kJipInstruction);
		const SIZE_T immediate = GetJIPAddress(kJipImmediate);
		const UInt32 vanillaDescription = static_cast<UInt32>(
			GetJIPAddress(kJipVanillaDescription));
		if (!hook_identity::IsAccessibleRegion(instruction, 5, true)
			|| *reinterpret_cast<const UInt8*>(instruction) != 0xBA
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

	static bool InstallDoorPromptHook(SIZE_T hook, const char* mode)
	{
		const std::array<Rel32Site, 1> sites = {{ kDoorPromptCallSite }};
		if (!ValidateVanillaCallSites(sites))
			return false;
		WriteRelCall(kDoorPromptCallSite.address, hook);
		if (hook_identity::MatchesRel32Target(
			kDoorPromptCallSite.address, Rel32Opcode::Call, hook))
		{
			return true;
		}
		SIZE_T observed = 0;
		const bool readable = hook_identity::ReadRel32Target(
			kDoorPromptCallSite.address, Rel32Opcode::Call, observed);
		gLog.FormattedMessage(
			"tnvse_font_hook: door prompt %s hook write verification failed observed=%08X readable=%u rollback=%s",
			mode,
			static_cast<UInt32>(observed),
			readable ? 1u : 0u,
			readable && observed == kDoorPromptCallSite.vanillaTarget
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
		if (!hook_identity::IsAccessibleRegion(
				kPluralBranch, sizeof(UInt8), true)
			|| *reinterpret_cast<const UInt8*>(kPluralBranch) != 0x74)
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
		SafeWrite8(kPluralBranch, 0xEB);
		const UInt8 observed = *reinterpret_cast<const UInt8*>(kPluralBranch);
		if (observed != 0xEB)
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: plural branch write verification failed observed=%02X rollback=%s",
				static_cast<UInt32>(observed),
				observed == 0x74
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

		// FontManager::CreateText -> FontManager::PrepText
		WriteRelCallEx(0xA18F4A, &FontManagerEx::PrepText);
		// FontManager::CreateText -> TextDoc::Render
		WriteRelCallEx(0xA18F63, &FontManagerEx::TextDocRender);
		// TextDoc::Render -> Font::AddChar
		WriteRelCallEx(0xA19622, &FontEx::TextDocRenderAddChar);
		// Terminal text needs the custom single-byte FreeType preparation path
		// even when the global DBCS parser is disabled. Non-FreeType fonts are
		// delegated by FontEx::PrepTextForTerminal.
		WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);
		if (g_bEnableMultibyteFontHook)
		{
			// AnimatingText::Update normally treats its elapsed-interval count as
			// a byte count.  Interpret it as encoded units at the single memcpy
			// call site so a DBCS lead byte is never published on its own.
			WriteRelCall(0x6FFFEE, &CopyAnimatingTextEncodedUnits);
		}
		// Feed final FreeType widths into word wrapping before TextLine chooses
		// whether to retain the character, move a word, or create another line.
		WriteRelCallEx(0xA19C80, &FontManagerEx::TextLineAddChar);

		if (!g_bEnableMultibyteFontHook)
		{
			// TextLine's constructor inserts the first character through a
			// separate call site. Patch it only for FreeType-only mode so every
			// line starts with the same final-width contract, while the enabled
			// multibyte path remains byte-for-byte on its existing hook set.
			WriteRelCallEx(0xA1BDE2, &FontManagerEx::TextLineAddChar);
			FinalizeFontHookGraph(false, g_bEnableFreeTypeFontRendering);
			if (!s_fontHookInstallState.freeType)
				return s_fontHookInstallState;
			gLog.FormattedMessage(
				"tnvse_font_hook: installed mode=freetype-custom-single-byte configuredCodePage=%u freeTypeCodePage=%u",
				g_usingWinEncoding, GetFreeTypeTextCodePage());
			return s_fontHookInstallState;
		}

		WriteRelJumpEx(kFontPrepText, &FontEx::PrepText);

		// FontManager::PrepText -> FontManager::PrepHypertext
		WriteRelCallEx(0xA18ACC, &FontManagerEx::PrepHypertext);

		// FontManager::PrepHypertext -> CollectTo
		WriteRelCall(0xA1772D, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17835, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17A1E, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17B65, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17BB1, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17CFE, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17D5D, &FontManagerEx::CollectToAttributeValue);
		WriteRelCall(0xA17DE9, &FontManagerEx::CollectToAttributeValue);

		// FontManager::CreateText -> FontManager::TextDoc::~TextDoc
		WriteRelCallEx(0xA18F7D, &FontManagerEx::TextDocDestructor);

		// FontManager::PrepHypertext -> TextDoc::AddChar
		WriteRelCallEx(0xA178A4, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA179D9, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA17FC2, &FontManagerEx::TextDocAddChar);
		// FontManager::PrepText -> TextDoc::AddChar
		WriteRelCallEx(0xA18D7C, &FontManagerEx::TextDocAddChar);

		// TextDoc::AddChar -> TextPage::AddChar
		WriteRelCallEx(0xA19A6F, &FontManagerEx::TextPageAddChar);
		// TextPage::TextPage -> TextPage::AddChar
		WriteRelCallEx(0xA1BD1C, &FontManagerEx::TextPageAddChar);

		// FontManager::PrepHypertext -> CharData::Copy
		WriteRelCall(0xA17898, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA179CD, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA17FB6, &FontManagerEx::CharDataCopy);
		// FontManager::PrepText -> CharData::Copy
		WriteRelCall(0xA18D73, &FontManagerEx::CharDataCopy);

		// Tile::SetString - Quest Text
		WriteRelCall(0x77AF4B,
			&TileSetStringHookForQuestAndLocationText);
		// Tile::SetString - Location Text
		WriteRelCall(0x772B5E,
			&TileSetStringHookForQuestAndLocationText);

		// BSStringT<char>::c_str - Terminal UTF8 conversion
		WriteRelCall(0x7591AC, &BSString_c_strHook);

		// BSStringT<char>::GetCStringOrEmpty - Location Text UTF8 conversion
		WriteRelCall(0x772B4B, &BSString_GetCStringOrEmptyHook);

		// strcpy_s - Quest Text UTF8 conversion
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
