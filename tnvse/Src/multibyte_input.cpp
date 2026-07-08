#include "multibyte_input.h"

#include "encoding.h"
#include "InterfaceManager.hpp"
#include "load_config.h"
#include "NiDX9Renderer.hpp"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Windows.h>
#include <d3d9.h>
#include <Imm.h>
#include <msctf.h>
#include <OleAuto.h>

namespace fonthook
{
	namespace
	{
		constexpr SIZE_T kPlayerNameTextEditOpenCall = 0x7AB740;
		constexpr SIZE_T kPlayerNameIsValidName = 0x7AB820;
		constexpr SIZE_T kTextEditMenuVTable = 0x1070034;
		constexpr SIZE_T kTextEditMenuHandleKeyboardInput = 0x7E6620;
		constexpr SIZE_T kTextEditMenuInputVTableEntry = 0x1070064;
		constexpr SIZE_T kTextEditStateInputCallInHandleKeyboardInput = 0x7E6685;
		constexpr UInt32 kMaxTextEditRawBytes = 1023;
		constexpr UInt32 kJipNumericOnlyFlag = 1;
		constexpr UInt32 kJipEnterAcceptsOkFlag = 2;
		constexpr DWORD kDuplicateImeCharSuppressMs = 250;
		constexpr DWORD kDuplicateAsciiSuppressMs = 100;
		constexpr DWORD kNativeImeAsciiGuardMs = 1000;
		constexpr UInt32 kMaxImeCandidatesToDisplay = 9;
		constexpr UInt32 kMessage_OnFramePresent = NVSEMessagingInterface::kMessage_PostQueryPlugins + 1;
		constexpr UInt32 kOverlayPadding = 10;
		constexpr UInt32 kOverlayLineHeight = 24;
		constexpr UInt32 kOverlayMinWidth = 260;
		constexpr UInt32 kOverlayMaxWidth = 620;
		constexpr UInt32 kStewieTweaksMinVersion = 990;
		constexpr const char* kStewieTweaksPluginName = "lStewieAl's Tweaks";
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewMenu_SearchBar = 5;
		constexpr UInt32 kStewMenu_SubsettingInputFieldText = 103;
		constexpr UInt32 kStewieMenuSearch_TextTile = 87698483;
		constexpr UInt32 kStewieMaxShadowBytes = 1023;

		constexpr SIZE_T kInventoryMenuHandleKeyboardInputEntry = 0x10739E4;
		constexpr SIZE_T kStatsMenuHandleKeyboardInputEntry = 0x1070004;
		constexpr SIZE_T kMapMenuHandleKeyboardInputEntry = 0x1074D74;
		constexpr SIZE_T kContainerMenuHandleKeyboardInputEntry = 0x10721DC;
		constexpr SIZE_T kBarterMenuHandleKeyboardInputEntry = 0x107071C;
		constexpr SIZE_T kLevelUpMenuHandleKeyboardInputEntry = 0x1073D0C;
		constexpr SIZE_T kRecipeMenuHandleKeyboardInputEntry = 0x10704BC;
		constexpr SIZE_T kStartMenuHandleKeyboardInputEntry = 0x1076D4C;
		constexpr UInt32 kMenuHandleKeyboardInputVTableOffset = 0x30;

		constexpr UInt32 kInputCode_Backspace = 0x80000000;
		constexpr UInt32 kInputCode_ArrowLeft = 0x80000001;
		constexpr UInt32 kInputCode_ArrowRight = 0x80000002;
		constexpr UInt32 kInputCode_ArrowUp = 0x80000003;
		constexpr UInt32 kInputCode_ArrowDown = 0x80000004;
		constexpr UInt32 kInputCode_Home = 0x80000005;
		constexpr UInt32 kInputCode_End = 0x80000006;
		constexpr UInt32 kInputCode_Delete = 0x80000007;
		constexpr UInt32 kInputCode_Enter = 0x80000008;
		constexpr UInt32 kInputCode_PageUp = 0x80000009;
		constexpr UInt32 kInputCode_PageDown = 0x8000000A;

		HWND s_window = nullptr;
		WNDPROC s_originalWndProc = nullptr;
		bool s_initialized = false;
		bool s_hooksInstalled = false;
		bool s_imeComposing = false;
		bool s_compositionEchoChecked = false;
		DWORD s_lastImeCommitTick = 0;
		DWORD s_lastWndProcAsciiTick = 0;
		UInt8 s_lastWndProcAsciiChar = 0;
		UInt32 s_suppressedImeCharCount = 0;
		SIZE_T s_jipOriginalInputHandler = 0;

		enum class StewieInputKind : UInt8
		{
			None,
			StewMenuSearch,
			StewMenuStringSubsetting,
			MenuSearch,
		};

		struct StewieInputTarget
		{
			StewieInputKind kind = StewieInputKind::None;
			Menu* menu = nullptr;
			Tile* tile = nullptr;
			bool inputField = false;
			bool valid = false;
		};

		struct StewieShadowState
		{
			StewieInputTarget target;
			std::string text;
			size_t caret = 0;
			size_t appliedBytes = 0;
			bool initialized = false;
		};

		using StewieKeyboardHandler = bool(__thiscall*)(Menu*, UInt32);

		struct StewieMenuHook
		{
			const char* name = "";
			UInt32 menuID = 0;
			SIZE_T entry = 0;
			SIZE_T original = 0;
			SIZE_T hook = 0;
			bool installed = false;
		};

		struct ImeCandidateState
		{
			bool composing = false;
			bool imeOpen = false;
			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			DWORD selection = 0;
			DWORD pageStart = 0;
			DWORD pageSize = 0;
			bool candidatesFromTsf = false;
			std::wstring imeName;
			std::wstring composition;
			std::vector<std::wstring> candidates;
		};

		struct CandidateOverlayLine
		{
			std::wstring text;
			bool highlighted = false;
		};

		struct CandidateOverlayState
		{
			LPDIRECT3DTEXTURE9 texture = nullptr;
			UInt32 textureWidth = 0;
			UInt32 textureHeight = 0;
			std::wstring lastKey;
			bool dirty = true;
			bool visible = false;
		};

		ImeCandidateState s_imeCandidateState;
		CandidateOverlayState s_candidateOverlay;
		bool s_tsfCandidateActive = false;
		bool s_hidingSystemImeWindows = false;
		bool s_gameImeContextDetached = false;
		bool s_textInputSessionActive = false;
		DWORD s_nativeImeAsciiGuardUntilTick = 0;
		bool s_stewieChecked = false;
		bool s_stewieAvailable = false;
		bool s_stewieHooksInstalled = false;
		bool s_stewieReplay = false;
		SIZE_T s_stewMenuOriginalInputHandler = 0;
		SIZE_T s_stewMenuHookedEntry = 0;
		UInt32 s_tileTraitIsActive = 0;
		UInt32 s_tileTraitIsSearchActive = 0;
		UInt32 s_tileTraitCaretIndex = 0;
		StewieShadowState s_stewieShadow;

		class JipTextInputAdapterEx
		{
		public:
			static bool __fastcall Input(TextEditMenu* apMenu, void*, UInt32 aiInput);
		};

		class StewieTweaksInputTargetEx
		{
		public:
			static bool __fastcall StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall InventoryMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall StatsMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall MapMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall ContainerMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall BarterMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall LevelUpMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall RecipeMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
			static bool __fastcall StartMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		};

		void UpdateCandidateOverlay();
		void DrawCandidateOverlay();
		void ReleaseCandidateOverlayTexture();
		void HideCandidateOverlay();
		void ClearImeCandidates();
		TextEditMenu* GetAnyActiveTextInputMenu();
		TextEditMenu* GetOverlayTextInputMenu();
		StewieInputTarget GetActiveStewieInputTarget();
		StewieInputTarget GetOverlayStewieInputTarget();
		bool HasOverlayInputTarget();
		void TryInstallStewieTweaksInputHooks();
		void ClearStewieInputState();
		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text);
		bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text);
		bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead);
		void HideSystemImeWindows(HWND hwnd);
		void SetTextInputSessionActive(bool active);
		void SetGameImeEnabled(HWND hwnd, bool enable);
		void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout = nullptr);
		void EnsureConfiguredImeOpen(HWND hwnd, const char* reason, HKL expectedLayout = nullptr);
		void UpdateGameImeAssociation();
		bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout = nullptr);
		bool IsNativeImeAsciiGuardActive();
		bool IsImeCompositionActive();
		bool IsImeConsumingAscii();
		std::string WideToCurrentCodePage(std::wstring_view value);

		void DebugLog(const char* fmt, ...)
		{
			if (!g_bMultibyteInputDebug)
				return;

			va_list args;
			va_start(args, fmt);
			gLog.FormattedMessage(fmt, args);
			va_end(args);
		}

		StewieMenuHook s_stewieMenuHooks[] =
		{
			{ "InventoryMenu", Inventory, kInventoryMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::InventoryMenuKeyboardInput), false },
			{ "StatsMenu", Stats, kStatsMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StatsMenuKeyboardInput), false },
			{ "MapMenu", PipboyData, kMapMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::MapMenuKeyboardInput), false },
			{ "ContainerMenu", Container, kContainerMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::ContainerMenuKeyboardInput), false },
			{ "BarterMenu", Barter, kBarterMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::BarterMenuKeyboardInput), false },
			{ "LevelUpMenu", LevelUp, kLevelUpMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::LevelUpMenuKeyboardInput), false },
			{ "RecipeMenu", Recipe, kRecipeMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::RecipeMenuKeyboardInput), false },
			{ "StartMenu", Pause, kStartMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StartMenuKeyboardInput), false },
		};

		template <class T>
		void SafeRelease(T*& ptr)
		{
			if (ptr)
			{
				ptr->Release();
				ptr = nullptr;
			}
		}

		class TsfCandidateSink final : public ITfUIElementSink
		{
		public:
			TsfCandidateSink() = default;

			~TsfCandidateSink()
			{
				Shutdown();
			}

			STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
			{
				if (!ppvObj)
					return E_INVALIDARG;

				*ppvObj = nullptr;
				if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfUIElementSink))
				{
					*ppvObj = static_cast<ITfUIElementSink*>(this);
					AddRef();
					return S_OK;
				}

				return E_NOINTERFACE;
			}

			STDMETHODIMP_(ULONG) AddRef() override
			{
				return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
			}

			STDMETHODIMP_(ULONG) Release() override
			{
				const LONG result = InterlockedDecrement(&m_refCount);
				return static_cast<ULONG>(result);
			}

			STDMETHODIMP BeginUIElement(DWORD dwUIElementId, BOOL* pbShow) override
			{
				if (pbShow && g_bMultibyteInputHideSystemCandidateWindow && HasOverlayInputTarget())
					*pbShow = FALSE;

				ReadCandidateElement(dwUIElementId);
				UpdateCandidateOverlay();
				return S_OK;
			}

			STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override
			{
				ReadCandidateElement(dwUIElementId);
				UpdateCandidateOverlay();
				return S_OK;
			}

			STDMETHODIMP EndUIElement(DWORD) override
			{
				s_tsfCandidateActive = false;
				if (s_imeCandidateState.candidatesFromTsf)
					ClearImeCandidates();
				UpdateCandidateOverlay();
				return S_OK;
			}

			bool Initialize()
			{
				if (m_initialized)
					return true;

				const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				m_coInitialized = SUCCEEDED(coInit);
				if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE)
					return false;

				HRESULT hr = CoCreateInstance(
					CLSID_TF_ThreadMgr,
					nullptr,
					CLSCTX_INPROC_SERVER,
					__uuidof(ITfThreadMgrEx),
					reinterpret_cast<void**>(&m_threadMgrEx));
				if (FAILED(hr) || !m_threadMgrEx)
					return false;

				hr = m_threadMgrEx->ActivateEx(&m_clientId, TF_TMAE_UIELEMENTENABLEDONLY);
				if (FAILED(hr))
					return false;

				m_activated = true;

				ITfSource* source = nullptr;
				hr = m_threadMgrEx->QueryInterface(__uuidof(ITfSource), reinterpret_cast<void**>(&source));
				if (FAILED(hr) || !source)
					return false;

				hr = source->AdviseSink(__uuidof(ITfUIElementSink), static_cast<ITfUIElementSink*>(this), &m_uiElementSinkCookie);
				SafeRelease(source);
				if (FAILED(hr))
					return false;

				ITfInputProcessorProfiles* profiles = nullptr;
				if (SUCCEEDED(CoCreateInstance(
					CLSID_TF_InputProcessorProfiles,
					nullptr,
					CLSCTX_INPROC_SERVER,
					IID_ITfInputProcessorProfiles,
					reinterpret_cast<void**>(&profiles))) && profiles)
				{
					profiles->QueryInterface(IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&m_profileMgr));
					SafeRelease(profiles);
				}

				m_initialized = true;
				return true;
			}

			void Shutdown()
			{
				if (m_threadMgrEx)
				{
					ITfSource* source = nullptr;
					if (m_uiElementSinkCookie != TF_INVALID_COOKIE
						&& SUCCEEDED(m_threadMgrEx->QueryInterface(__uuidof(ITfSource), reinterpret_cast<void**>(&source)))
						&& source)
					{
						source->UnadviseSink(m_uiElementSinkCookie);
						SafeRelease(source);
					}

					if (m_activated)
						m_threadMgrEx->Deactivate();
				}

				m_uiElementSinkCookie = TF_INVALID_COOKIE;
				m_activated = false;
				m_initialized = false;
				SafeRelease(m_profileMgr);
				SafeRelease(m_threadMgrEx);

				if (m_coInitialized)
				{
					CoUninitialize();
					m_coInitialized = false;
				}
			}

			std::wstring GetCurrentInputMethodName() const
			{
				if (!m_profileMgr)
					return {};

				TF_INPUTPROCESSORPROFILE profile = {};
				if (FAILED(m_profileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)))
					return {};

				if (profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR)
					return {};

				ITfInputProcessorProfiles* profiles = nullptr;
				if (FAILED(CoCreateInstance(
					CLSID_TF_InputProcessorProfiles,
					nullptr,
					CLSCTX_INPROC_SERVER,
					IID_ITfInputProcessorProfiles,
					reinterpret_cast<void**>(&profiles))) || !profiles)
					return {};

				BSTR description = nullptr;
				std::wstring result;
				if (SUCCEEDED(profiles->GetLanguageProfileDescription(
					profile.clsid,
					profile.langid,
					profile.guidProfile,
					&description)) && description)
				{
					result = description;
					SysFreeString(description);
				}

				SafeRelease(profiles);
				return result;
			}

		private:
			ITfUIElement* GetUIElement(DWORD id) const
			{
				if (!m_threadMgrEx)
					return nullptr;

				ITfUIElementMgr* manager = nullptr;
				ITfUIElement* element = nullptr;
				if (SUCCEEDED(m_threadMgrEx->QueryInterface(__uuidof(ITfUIElementMgr), reinterpret_cast<void**>(&manager))) && manager)
				{
					manager->GetUIElement(id, &element);
					SafeRelease(manager);
				}
				return element;
			}

			void ReadCandidateElement(DWORD id)
			{
				ITfUIElement* element = GetUIElement(id);
				if (!element)
					return;

				ITfCandidateListUIElement* candidate = nullptr;
				if (SUCCEEDED(element->QueryInterface(__uuidof(ITfCandidateListUIElement), reinterpret_cast<void**>(&candidate))) && candidate)
				{
					ReadCandidateList(candidate);
					SafeRelease(candidate);
				}

				SafeRelease(element);
			}

			void ReadCandidateList(ITfCandidateListUIElement* candidate)
			{
				UINT selection = 0;
				UINT count = 0;
				UINT currentPage = 0;
				UINT pageCount = 0;
				if (FAILED(candidate->GetCount(&count)) || !count)
					return;

				if (FAILED(candidate->GetSelection(&selection)))
					selection = 0;

				if (FAILED(candidate->GetCurrentPage(&currentPage)))
					currentPage = 0;

				DWORD pageStart = 0;
				DWORD pageSize = count;
				if (SUCCEEDED(candidate->GetPageIndex(nullptr, 0, &pageCount)) && pageCount)
				{
					std::vector<UINT> pageIndexes(pageCount);
					if (SUCCEEDED(candidate->GetPageIndex(pageIndexes.data(), pageCount, &pageCount))
						&& currentPage < pageCount)
					{
						pageStart = pageIndexes[currentPage];
						const DWORD nextPageStart = currentPage + 1 < pageCount
							? pageIndexes[currentPage + 1]
							: count;
						pageSize = nextPageStart > pageStart ? nextPageStart - pageStart : count - pageStart;
					}
				}

				pageSize = std::min<DWORD>(pageSize, kMaxImeCandidatesToDisplay);

				std::vector<std::wstring> candidates;
				candidates.reserve(pageSize);
				for (DWORD i = 0; i < pageSize; ++i)
				{
					BSTR value = nullptr;
					if (SUCCEEDED(candidate->GetString(pageStart + i, &value)) && value)
					{
						candidates.emplace_back(value);
						SysFreeString(value);
					}
				}

				if (candidates.empty())
					return;

				s_tsfCandidateActive = true;
				s_imeCandidateState.candidatesFromTsf = true;
				s_imeCandidateState.selection = selection;
				s_imeCandidateState.pageStart = pageStart;
				s_imeCandidateState.pageSize = pageSize;
				s_imeCandidateState.candidates = std::move(candidates);
			}

			LONG m_refCount = 1;
			bool m_initialized = false;
			bool m_activated = false;
			bool m_coInitialized = false;
			TfClientId m_clientId = TF_CLIENTID_NULL;
			DWORD m_uiElementSinkCookie = TF_INVALID_COOKIE;
			ITfThreadMgrEx* m_threadMgrEx = nullptr;
			ITfInputProcessorProfileMgr* m_profileMgr = nullptr;
		};

		std::unique_ptr<TsfCandidateSink> s_tsfCandidateSink;

		char PrintableAscii(UInt32 value)
		{
			return (value >= 0x20 && value <= 0x7E) ? static_cast<char>(value) : '.';
		}

		bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs)
		{
			if (lhs > 0x7F || rhs > 0x7F)
				return false;

			return std::tolower(lhs) == std::tolower(static_cast<unsigned char>(rhs));
		}

		void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input)
		{
			if (!g_bMultibyteInputDebug)
				return;

			const UInt32 textLen = menu ? menu->xEditState.xText.GetLength() : 0;
			const UInt32 caret = menu ? menu->xEditState.iCaretByteOffset : 0;
			gLog.FormattedMessage(
				"tnvse_multibyte_input_event: source=%s action=%s input=0x%08X ascii='%c' composing=%u active=0x%08X current=0x%08X caret=%u textLen=%u",
				source,
				action,
				static_cast<UInt32>(input),
				PrintableAscii(static_cast<UInt32>(input)),
				s_imeComposing ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(TextEditMenu::GetCurrent()),
				caret,
				textLen);
		}

		std::string GetText(const TextEditState& state)
		{
			const UInt32 length = state.xText.GetLength();
			if (!length)
				return {};

			return std::string(state.xText.c_str(), length);
		}

		size_t NextOffset(const std::string& text, size_t offset)
		{
			if (offset >= text.size())
				return text.size();

			UInt32 code = 0;
			if (offset + 1 < text.size() && TryDecodeDoubleByte(&text[offset], code))
				return offset + 2;

			return offset + 1;
		}

		bool IsCharBoundary(const std::string& text, size_t offset)
		{
			if (offset > text.size())
				return false;

			size_t current = 0;
			while (current < text.size())
			{
				if (current == offset)
					return true;

				current = NextOffset(text, current);
			}

			return offset == text.size();
		}

		size_t PrevCharBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			size_t previous = 0;
			size_t current = 0;
			while (current < offset)
			{
				previous = current;
				current = NextOffset(text, current);
			}

			return previous;
		}

		size_t ClampToPrevBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			if (IsCharBoundary(text, offset))
				return offset;

			return PrevCharBoundary(text, offset);
		}

		size_t NextCharBoundary(const std::string& text, size_t offset)
		{
			offset = ClampToPrevBoundary(text, offset);
			return NextOffset(text, offset);
		}

		bool IsCtrlKeyDown()
		{
			return (GetKeyState(VK_CONTROL) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		}

		UInt32 TileID(Tile* tile)
		{
			if (!tile)
				return 0;

			return static_cast<UInt32>(tile->GetValueFloat(Tile::kTileValue_id));
		}

		float TileTraitFloat(Tile* tile, UInt32 trait)
		{
			return tile && trait ? tile->GetValueFloat(trait) : 0.0f;
		}

		Tile* MenuRoot(Menu* menu)
		{
			return menu ? reinterpret_cast<Tile*>(menu->pRootTile) : nullptr;
		}

		UInt32 MenuID(Menu* menu)
		{
			return menu ? menu->GetID() : 0;
		}

		Menu* GetOpenMenu(UInt32 menuID)
		{
			Tile* root = InterfaceManager::GetMenuByType(menuID);
			if (!root)
				return nullptr;

			Menu* menu = root->GetMenu();
			if (!menu || MenuID(menu) != menuID)
				return nullptr;

			return menu;
		}

		Tile* FindTileByID(Tile* tile, UInt32 id)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id)
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindTileByID(child, id))
					return result;
			}

			return nullptr;
		}

		Tile* FindStewieActiveInputTile(Tile* tile, UInt32 id)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindStewieActiveInputTile(child, id))
					return result;
			}

			return nullptr;
		}

		std::string TileStringWithoutCaret(Tile* tile, size_t& caret)
		{
			caret = 0;
			std::string text = tile ? tile->GetValueString(Tile::kTileValue_string) : "";
			const size_t caretMarker = text.find('|');
			if (caretMarker != std::string::npos)
			{
				text.erase(caretMarker, 1);
				caret = ClampToPrevBoundary(text, caretMarker);
				return text;
			}

			if (s_tileTraitCaretIndex && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
			{
				const size_t caretIndex = static_cast<size_t>(TileTraitFloat(tile, s_tileTraitCaretIndex));
				if (caretIndex < text.size())
				{
					text.erase(caretIndex, 1);
					caret = ClampToPrevBoundary(text, caretIndex);
					return text;
				}
			}

			caret = text.size();
			return text;
		}

		bool SameStewieTarget(const StewieInputTarget& lhs, const StewieInputTarget& rhs)
		{
			return lhs.valid
				&& rhs.valid
				&& lhs.kind == rhs.kind
				&& lhs.menu == rhs.menu
				&& lhs.tile == rhs.tile
				&& lhs.inputField == rhs.inputField;
		}

		bool IsStewieTweaksAvailable()
		{
			if (!g_bMultibyteInputStewieTweaks || !g_cmdTableInterface)
				return false;

			if (s_stewieChecked)
				return s_stewieAvailable;

			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByName(kStewieTweaksPluginName);
			if (!info)
				return false;

			s_stewieChecked = true;
			s_stewieAvailable = info
				&& info->version >= kStewieTweaksMinVersion;
			if (info && !s_stewieAvailable)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie Tweaks version %u is older than supported minimum %u; Stewie input adapter disabled",
					info->version,
					kStewieTweaksMinVersion);
			}
			else if (s_stewieAvailable)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie Tweaks version %u detected",
					info->version);
			}

			return s_stewieAvailable;
		}

		bool LooksLikeActiveMenuSearchTile(Tile* tile)
		{
			if (!tile)
				return false;

			if (s_tileTraitIsActive && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
				return true;

			const float visible = tile->GetValueFloat(Tile::kTileValue_visible);
			const float alpha = tile->GetValueFloat(Tile::kTileValue_alpha);
			return visible > 0.5f && alpha > 200.0f;
		}

		StewieInputTarget MakeStewieTarget(StewieInputKind kind, Menu* menu, Tile* tile, bool inputField)
		{
			StewieInputTarget target;
			target.kind = kind;
			target.menu = menu;
			target.tile = tile;
			target.inputField = inputField;
			target.valid = menu && tile;
			return target;
		}

		bool TryGetStewieInputFieldType(Menu* menu, Tile* tile, UInt8& inputType)
		{
			if (!menu || !tile)
				return false;

			static constexpr UInt32 kInputFieldStringLengthOffset = 0x08;
			static constexpr UInt32 kInputFieldStringCapacityOffset = 0x0A;
			static constexpr UInt32 kInputFieldActiveOffset = 0x0C;
			static constexpr UInt32 kInputFieldCaretOffset = 0x0E;
			static constexpr UInt32 kInputFieldTypeOffset = 0x14;

			__try
			{
				auto* base = reinterpret_cast<UInt8*>(menu);
				for (UInt32 offset = 0; offset < 0x2000; offset += sizeof(void*))
				{
					if (*reinterpret_cast<Tile**>(base + offset) != tile)
						continue;

					const bool isActive = *reinterpret_cast<bool*>(base + offset + kInputFieldActiveOffset);
					const UInt16 length = *reinterpret_cast<UInt16*>(base + offset + kInputFieldStringLengthOffset);
					const UInt16 capacity = *reinterpret_cast<UInt16*>(base + offset + kInputFieldStringCapacityOffset);
					const SInt16 caret = *reinterpret_cast<SInt16*>(base + offset + kInputFieldCaretOffset);
					const UInt8 candidateType = *reinterpret_cast<UInt8*>(base + offset + kInputFieldTypeOffset);
					if (isActive
						&& candidateType <= 3
						&& capacity < 0x10000
						&& length <= capacity
						&& caret >= 0
						&& static_cast<UInt16>(caret) <= length)
					{
						inputType = candidateType;
						return true;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			return false;
		}

		Tile* FindStewieInputFieldTile(Menu* menu, Tile* tile, UInt32 id, UInt8& inputType)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id && TryGetStewieInputFieldType(menu, tile, inputType))
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindStewieInputFieldTile(menu, child, id, inputType))
					return result;
			}

			return nullptr;
		}

		StewieInputTarget FindStewMenuTarget(Menu* menu)
		{
			if (!menu || MenuID(menu) != kMenuType_StewMenu)
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			UInt8 searchInputType = 0xFF;
			if (Tile* searchInputTile = FindStewieInputFieldTile(menu, root, kStewMenu_SearchBar, searchInputType);
				searchInputTile && searchInputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchInputTile, true);

			Tile* searchTile = FindTileByID(root, kStewMenu_SearchBar);
			const float searchRootActive = TileTraitFloat(root, s_tileTraitIsSearchActive);
			const float searchTileActive = TileTraitFloat(searchTile, s_tileTraitIsActive);
			if (searchTile
				&& (searchTileActive > 0.5f
					|| (searchRootActive > 0.5f && searchRootActive < 1.5f)))
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchTile, true);

			UInt8 inputType = 0xFF;
			Tile* subsettingTile = FindStewieInputFieldTile(menu, root, kStewMenu_SubsettingInputFieldText, inputType);
			if (subsettingTile && inputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuStringSubsetting, menu, subsettingTile, true);

			return {};
		}

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu)
		{
			if (!menu)
				return {};

			Tile* searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
			if (!LooksLikeActiveMenuSearchTile(searchTile))
				return {};

			const bool inputField = s_tileTraitIsActive && TileTraitFloat(searchTile, s_tileTraitIsActive) > 0.5f;
			return MakeStewieTarget(StewieInputKind::MenuSearch, menu, searchTile, inputField);
		}

		StewieInputTarget FindStewieTargetForMenu(Menu* menu)
		{
			if (!IsStewieTweaksAvailable() || !menu)
				return {};

			if (MenuID(menu) == kMenuType_StewMenu)
				return FindStewMenuTarget(menu);

			return FindStewieMenuSearchTarget(menu);
		}

		StewieInputTarget GetActiveStewieInputTarget()
		{
			if (!IsStewieTweaksAvailable())
				return {};

			if (Menu* menu = GetOpenMenu(kMenuType_StewMenu))
			{
				if (StewieInputTarget target = FindStewMenuTarget(menu); target.valid)
					return target;
			}

			for (const StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (StewieInputTarget target = FindStewieMenuSearchTarget(menu); target.valid)
						return target;
				}
			}

			return {};
		}

		StewieInputTarget GetOverlayStewieInputTarget()
		{
			if (StewieInputTarget target = GetActiveStewieInputTarget(); target.valid)
				return target;

			if (s_stewieShadow.initialized)
			{
				StewieInputTarget target = FindStewieTargetForMenu(s_stewieShadow.target.menu);
				if (target.valid)
					return target;
			}

			return {};
		}

		bool HasOverlayInputTarget()
		{
			return GetOverlayTextInputMenu() || GetOverlayStewieInputTarget().valid;
		}

		SIZE_T OriginalStewieHandlerForMenu(Menu* menu)
		{
			if (!menu)
				return 0;

			const UInt32 menuID = MenuID(menu);
			if (menuID == kMenuType_StewMenu)
				return s_stewMenuOriginalInputHandler;

			for (const StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (hook.menuID == menuID)
					return hook.original;
			}

			return 0;
		}

		bool CallStewieOriginalInput(Menu* menu, UInt32 input)
		{
			const SIZE_T original = OriginalStewieHandlerForMenu(menu);
			if (!original)
				return false;

			return reinterpret_cast<StewieKeyboardHandler>(original)(menu, input);
		}

		void EnsureStewieShadow(const StewieInputTarget& target)
		{
			if (SameStewieTarget(s_stewieShadow.target, target))
				return;

			s_stewieShadow.target = target;
			s_stewieShadow.text = TileStringWithoutCaret(target.tile, s_stewieShadow.caret);
			if (!target.inputField && s_stewieShadow.text == "_")
				s_stewieShadow.text.clear();
			s_stewieShadow.caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			s_stewieShadow.appliedBytes = s_stewieShadow.text.size();
			s_stewieShadow.initialized = target.valid;
		}

		bool ReplayStewieShadow(const StewieInputTarget& target)
		{
			if (!target.valid || !s_stewieShadow.initialized)
				return false;

			const size_t clearCount = std::min<size_t>(s_stewieShadow.appliedBytes, kStewieMaxShadowBytes);

			s_stewieReplay = true;
			CallStewieOriginalInput(target.menu, kInputCode_End);
			for (size_t i = 0; i < clearCount; ++i)
				CallStewieOriginalInput(target.menu, kInputCode_Backspace);

			for (unsigned char ch : s_stewieShadow.text)
				CallStewieOriginalInput(target.menu, ch);

			const size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			for (size_t i = caret; i < s_stewieShadow.text.size(); ++i)
				CallStewieOriginalInput(target.menu, kInputCode_ArrowLeft);
			s_stewieReplay = false;

			s_stewieShadow.appliedBytes = s_stewieShadow.text.size();
			return true;
		}

		bool FitsStewieShadow(const std::string& candidate)
		{
			return candidate.size() <= kStewieMaxShadowBytes;
		}

		bool CommitStewieShadow(const StewieInputTarget& target, std::string candidate, size_t caret)
		{
			if (!FitsStewieShadow(candidate))
				return false;

			s_stewieShadow.target = target;
			s_stewieShadow.text = std::move(candidate);
			s_stewieShadow.caret = ClampToPrevBoundary(s_stewieShadow.text, caret);
			s_stewieShadow.initialized = true;
			return ReplayStewieShadow(target);
		}

		bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text)
		{
			if (!target.valid || text.empty())
				return false;

			EnsureStewieShadow(target);
			std::string candidate = s_stewieShadow.text;
			size_t caret = ClampToPrevBoundary(candidate, s_stewieShadow.caret);
			candidate.insert(caret, text.data(), text.size());
			return CommitStewieShadow(target, std::move(candidate), caret + text.size());
		}

		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text)
		{
			std::string converted = WideToCurrentCodePage(text);
			if (converted.empty())
				return false;

			return InsertTextAtCaretStewie(target, converted);
		}

		bool DeletePreviousStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(previous, caret - previous);
			return CommitStewieShadow(target, std::move(candidate), previous);
		}

		bool DeleteNextStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (caret >= s_stewieShadow.text.size())
				return true;

			const size_t next = NextCharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(caret, next - caret);
			return CommitStewieShadow(target, std::move(candidate), caret);
		}

		bool MoveStewieCaretPrevious(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, PrevCharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretNext(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, NextCharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretHome(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, 0);
		}

		bool MoveStewieCaretEnd(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, s_stewieShadow.text.size());
		}

		bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead)
		{
			StewieInputTarget target = GetOverlayStewieInputTarget();
			if (!target.valid)
				return false;

			EnsureStewieShadow(target);
			size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return false;

			const size_t previous = PrevCharBoundary(s_stewieShadow.text, caret);
			if (caret - previous != 1)
				return false;

			if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(s_stewieShadow.text[previous]), compositionLead))
				return false;

			std::string candidate = s_stewieShadow.text;
			candidate.erase(previous, 1);
			return CommitStewieShadow(target, std::move(candidate), previous);
		}

		void ClearStewieInputState()
		{
			s_stewieShadow = StewieShadowState();
		}

		bool HandleStewieInput(Menu* menu, UInt32 input)
		{
			if (s_stewieReplay)
				return CallStewieOriginalInput(menu, input);

			StewieInputTarget target = FindStewieTargetForMenu(menu);
			if (!target.valid)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=target_miss menu=%u input=0x%08X",
					MenuID(menu),
					input);
				if (MenuID(menu) == kMenuType_StewMenu)
				{
					Tile* root = MenuRoot(menu);
					Tile* searchTile = FindTileByID(root, kStewMenu_SearchBar);
					UInt8 searchInputType = 0xFF;
					Tile* searchInputTile = FindStewieInputFieldTile(menu, root, kStewMenu_SearchBar, searchInputType);
					UInt8 subsettingInputType = 0xFF;
					Tile* subsettingTile = FindStewieInputFieldTile(menu, root, kStewMenu_SubsettingInputFieldText, subsettingInputType);
					DebugLog(
						"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=target_miss_stewmenu root=0x%08X search=0x%08X searchInput=0x%08X searchType=%u searchRootActive=%.1f searchTileActive=%.1f subsetting=0x%08X subsettingType=%u",
						reinterpret_cast<UInt32>(root),
						reinterpret_cast<UInt32>(searchTile),
						reinterpret_cast<UInt32>(searchInputTile),
						searchInputType,
						TileTraitFloat(root, s_tileTraitIsSearchActive),
						TileTraitFloat(searchTile, s_tileTraitIsActive),
						reinterpret_cast<UInt32>(subsettingTile),
						subsettingInputType);
				}
				return CallStewieOriginalInput(menu, input);
			}

			EnsureStewieShadow(target);

			if (IsCtrlKeyDown())
			{
				const bool handled = CallStewieOriginalInput(menu, input);
				ClearStewieInputState();
				return handled;
			}

			if (IsImeCompositionActive())
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=suppress_composition_input menu=%u input=0x%08X",
					MenuID(menu),
					input);
				return true;
			}

			if (input >= 0x20 && input <= 0x7E)
			{
				if (IsImeConsumingAscii())
					return true;

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(input)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					return true;
				}

				const char ch = static_cast<char>(input);
				return InsertTextAtCaretStewie(target, std::string_view(&ch, 1));
			}

			if (input > 0x7F && input <= 0xFF)
				return true;

			switch (input)
			{
			case kInputCode_Backspace:
				return DeletePreviousStewieChar(target);
			case kInputCode_Delete:
				return DeleteNextStewieChar(target);
			case kInputCode_ArrowLeft:
				return MoveStewieCaretPrevious(target);
			case kInputCode_ArrowRight:
				return MoveStewieCaretNext(target);
			case kInputCode_Home:
				return MoveStewieCaretHome(target);
			case kInputCode_End:
				return MoveStewieCaretEnd(target);
			case kInputCode_Enter:
				ClearStewieInputState();
				return CallStewieOriginalInput(menu, input);
			default:
				return CallStewieOriginalInput(menu, input);
			}
		}

		bool InstallStewieHook(StewieMenuHook& hook)
		{
			if (hook.installed)
				return true;

			SIZE_T current = *reinterpret_cast<SIZE_T*>(hook.entry);
			if (current == hook.hook)
			{
				hook.installed = true;
				return true;
			}

			hook.original = current;
			SafeWrite32(hook.entry, hook.hook);
			hook.installed = true;
			DebugLog(
				"tnvse_multibyte_input: chained Stewie %s handler=0x%08X",
				hook.name,
				static_cast<UInt32>(current));
			return true;
		}

		void TryInstallStewieTweaksInputHooks()
		{
			if (!IsStewieTweaksAvailable())
				return;

			if (!s_tileTraitIsActive)
				s_tileTraitIsActive = Tile::TraitNameToID("_IsActive");
			if (!s_tileTraitIsSearchActive)
				s_tileTraitIsSearchActive = Tile::TraitNameToID("_IsSearchActive");
			if (!s_tileTraitCaretIndex)
				s_tileTraitCaretIndex = Tile::TraitNameToID("_CaretIndex");

			if (!s_stewieHooksInstalled)
			{
				for (StewieMenuHook& hook : s_stewieMenuHooks)
					InstallStewieHook(hook);

				s_stewieHooksInstalled = true;
				gLog.FormattedMessage("tnvse_multibyte_input: Stewie Tweaks menu search input adapter installed");
			}

			if (Menu* stewMenu = GetOpenMenu(kMenuType_StewMenu))
			{
				SIZE_T entry = *reinterpret_cast<SIZE_T*>(stewMenu) + kMenuHandleKeyboardInputVTableOffset;
				SIZE_T current = *reinterpret_cast<SIZE_T*>(entry);
				const SIZE_T hook = reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StewMenuKeyboardInput);
				if (current != hook)
				{
					s_stewMenuOriginalInputHandler = current;
					s_stewMenuHookedEntry = entry;
					SafeWrite32(entry, hook);
					DebugLog(
						"tnvse_multibyte_input: chained StewMenu handler=0x%08X",
						static_cast<UInt32>(current));
					gLog.FormattedMessage("tnvse_multibyte_input: Stewie Tweaks StewMenu input adapter installed");
				}
			}
		}

		void SetCaret(TextEditState& state, size_t offset)
		{
			state.iCaretByteOffset = static_cast<UInt32>(std::min<size_t>(
				offset,
				std::numeric_limits<UInt32>::max()));
			state.bCaretVisible = true;
		}

		bool FitsTextEditConstraints(TextEditState& state, const std::string& candidate)
		{
			if (candidate.size() > kMaxTextEditRawBytes)
				return false;

			if (state.iMaxPixelWidth != -1 && !state.FitsMaxPixelWidth(candidate.c_str()))
				return false;

			return true;
		}

		TextEditMenu* GetCurrentTextEditMenuObject()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!current || *reinterpret_cast<SIZE_T*>(current) != kTextEditMenuVTable)
				return nullptr;

			return current;
		}

		TextEditMenu* GetActiveTextEditMenu()
		{
			TextEditMenu* current = GetCurrentTextEditMenuObject();
			if (!current)
				return nullptr;

			if (*reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry) != kTextEditMenuHandleKeyboardInput)
				return nullptr;

			if (!current->xEditState.IsActive())
				return nullptr;

			return current;
		}

		SIZE_T CurrentTextEditInputHandler()
		{
			return *reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry);
		}

		SIZE_T JipTextInputHandlerAddress()
		{
			return reinterpret_cast<SIZE_T>(&JipTextInputAdapterEx::Input);
		}

		BSStringT<char>& JipCurrentText(TextEditMenu* menu)
		{
			return *reinterpret_cast<BSStringT<char>*>(reinterpret_cast<UInt8*>(menu) + 0x34);
		}

		BSStringT<char>& JipDisplayedText(TextEditMenu* menu)
		{
			return *reinterpret_cast<BSStringT<char>*>(reinterpret_cast<UInt8*>(menu) + 0x3C);
		}

		UInt32& JipCursorIndex(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(menu) + 0x44);
		}

		UInt16 JipMinLength(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt16*>(reinterpret_cast<UInt8*>(menu) + 0x48);
		}

		UInt16 JipMaxLength(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt16*>(reinterpret_cast<UInt8*>(menu) + 0x4A);
		}

		Tile* JipInputRect(TextEditMenu* menu)
		{
			return *reinterpret_cast<Tile**>(reinterpret_cast<UInt8*>(menu) + 0x4C);
		}

		UInt8& JipCursorVisible(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x54);
		}

		UInt8 JipIsActiveFlag(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x55);
		}

		UInt8 JipMiscFlags(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x57);
		}

		bool LooksLikeJipTextInputStorage(TextEditMenu* menu)
		{
			if (!menu || *reinterpret_cast<SIZE_T*>(menu) != kTextEditMenuVTable)
				return false;

			const SIZE_T handler = CurrentTextEditInputHandler();
			if (handler == kTextEditMenuHandleKeyboardInput)
				return false;

			if (!JipCurrentText(menu).GetMaxLength() || !JipDisplayedText(menu).GetMaxLength())
				return false;

			const UInt16 maxLength = JipMaxLength(menu);
			if (!maxLength || maxLength > 0x7FFF)
				return false;

			const auto inputRect = reinterpret_cast<SIZE_T>(JipInputRect(menu));
			return inputRect > 0x10000;
		}

		bool LooksLikeJipTextInput(TextEditMenu* menu)
		{
			return LooksLikeJipTextInputStorage(menu) && JipIsActiveFlag(menu);
		}

		TextEditMenu* GetCurrentJipTextInputMenu()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			return LooksLikeJipTextInputStorage(current) ? current : nullptr;
		}

		TextEditMenu* GetActiveJipTextInputMenu()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			return LooksLikeJipTextInput(current) ? current : nullptr;
		}

		TextEditMenu* GetAnyActiveTextInputMenu()
		{
			if (TextEditMenu* menu = GetActiveTextEditMenu())
				return menu;

			return GetActiveJipTextInputMenu();
		}

		TextEditMenu* GetOverlayTextInputMenu()
		{
			if (TextEditMenu* menu = GetAnyActiveTextInputMenu())
				return menu;

			TextEditMenu* current = GetCurrentTextEditMenuObject();
			if (!current)
				return nullptr;

			return current;
		}

		void ClearJipTextInputHookState()
		{
			s_jipOriginalInputHandler = 0;
		}

		bool CallJipOriginalInput(TextEditMenu* menu, UInt32 input)
		{
			if (!menu || !s_jipOriginalInputHandler || s_jipOriginalInputHandler == JipTextInputHandlerAddress())
				return false;

			using InputHandler = bool(__thiscall*)(TextEditMenu*, UInt32);
			return reinterpret_cast<InputHandler>(s_jipOriginalInputHandler)(menu, input);
		}

		void TryInstallJipTextInputHook()
		{
			const SIZE_T currentHandler = CurrentTextEditInputHandler();
			const SIZE_T hookHandler = JipTextInputHandlerAddress();

			if (currentHandler == kTextEditMenuHandleKeyboardInput)
			{
				ClearJipTextInputHookState();
				return;
			}

			if (currentHandler == hookHandler)
				return;

			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!LooksLikeJipTextInput(current))
				return;

			s_jipOriginalInputHandler = currentHandler;
			SafeWrite32(kTextEditMenuInputVTableEntry, hookHandler);
			DebugLog(
				"tnvse_multibyte_input: chained JIP TextInput handler=0x%08X menu=0x%08X",
				static_cast<UInt32>(currentHandler),
				reinterpret_cast<UInt32>(current));
		}

		std::string GetJipText(TextEditMenu* menu)
		{
			BSStringT<char>& text = JipCurrentText(menu);
			const UInt32 length = text.GetLength();
			if (!length)
				return {};

			return std::string(text.c_str(), length);
		}

		void DebugLogJipState(const char* source, const char* action, TextEditMenu* menu, UInt32 input)
		{
			if (!g_bMultibyteInputDebug)
				return;

			const UInt32 textLen = menu ? JipCurrentText(menu).GetLength() : 0;
			const UInt32 caret = menu ? JipCursorIndex(menu) : 0;
			gLog.FormattedMessage(
				"tnvse_multibyte_input_event: source=%s action=%s input=0x%08X ascii='%c' composing=%u jip=1 active=0x%08X current=0x%08X caret=%u textLen=%u min=%u max=%u flags=0x%02X handler=0x%08X",
				source,
				action,
				input,
				PrintableAscii(input),
				s_imeComposing ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(TextEditMenu::GetCurrent()),
				caret,
				textLen,
				menu ? JipMinLength(menu) : 0,
				menu ? JipMaxLength(menu) : 0,
				menu ? JipMiscFlags(menu) : 0,
				static_cast<UInt32>(CurrentTextEditInputHandler()));
		}

		bool JipNumericInsertIsValid(TextEditMenu* menu, std::string_view text, const std::string& current, size_t caret)
		{
			if (!(JipMiscFlags(menu) & kJipNumericOnlyFlag))
				return true;

			for (char raw : text)
			{
				const UInt8 ch = static_cast<UInt8>(raw);
				if (ch >= 0x80)
					return false;

				if (ch == '-')
				{
					if (caret != 0 || current.find('-') != std::string::npos)
						return false;
					continue;
				}

				if (ch == '.')
				{
					if (current.find('.') != std::string::npos)
						return false;
					continue;
				}

				if (!std::isdigit(ch))
					return false;
			}

			return true;
		}

		void RefreshJipTextInput(TextEditMenu* menu)
		{
			if (!menu)
				return;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			JipCursorIndex(menu) = static_cast<UInt32>(caret);

			std::string displayText;
			displayText.reserve(current.size() + 1);
			displayText.append(current, 0, caret);
			displayText.push_back(JipCursorVisible(menu) ? '|' : '\x7F');
			displayText.append(current, caret, std::string::npos);
			JipDisplayedText(menu).Set(displayText.c_str());

			if (menu->pEditText)
				menu->pEditText->SetValueString(Tile::kTileValue_string, JipDisplayedText(menu).c_str(), true);

			if (menu->pOkButton)
			{
				const bool enabled = JipCurrentText(menu).GetLength() >= JipMinLength(menu);
				menu->pOkButton->SetValueFloat(Tile::kTileValue_target, enabled ? 1.0f : 0.0f, true);
			}

			if (Tile* inputRect = JipInputRect(menu))
			{
				const float user1 = inputRect->GetValueFloat(Tile::kTileValue_user1);
				inputRect->SetValueFloat(Tile::kTileValue_user2, user1, true);
			}
		}

		bool CommitJipCandidate(TextEditMenu* menu, const std::string& candidate, size_t caret)
		{
			if (!menu)
				return false;

			const UInt32 maxLength = std::min<UInt32>(JipMaxLength(menu), kMaxTextEditRawBytes);
			if (candidate.size() > maxLength)
				return false;

			JipCurrentText(menu).Set(candidate.c_str());
			JipCursorIndex(menu) = static_cast<UInt32>(ClampToPrevBoundary(candidate, caret));
			RefreshJipTextInput(menu);
			return true;
		}

		bool InsertJipTextAtCaret(TextEditMenu* menu, std::string_view text)
		{
			if (!menu || text.empty())
				return false;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!JipNumericInsertIsValid(menu, text, current, caret))
				return false;

			std::string candidate;
			candidate.reserve(current.size() + text.size());
			candidate.append(current, 0, caret);
			candidate.append(text.data(), text.size());
			candidate.append(current, caret, std::string::npos);
			return CommitJipCandidate(menu, candidate, caret + text.size());
		}

		bool DeletePreviousJipChar(TextEditMenu* menu)
		{
			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(current, caret);
			current.erase(previous, caret - previous);
			return CommitJipCandidate(menu, current, previous);
		}

		bool DeleteNextJipChar(TextEditMenu* menu)
		{
			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (caret >= current.size())
				return true;

			const size_t next = NextCharBoundary(current, caret);
			current.erase(caret, next - caret);
			return CommitJipCandidate(menu, current, caret);
		}

		bool MoveJipCaret(TextEditMenu* menu, size_t caret)
		{
			const std::string current = GetJipText(menu);
			JipCursorIndex(menu) = static_cast<UInt32>(ClampToPrevBoundary(current, caret));
			RefreshJipTextInput(menu);
			return true;
		}

		bool MoveJipCaretPrevious(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			const size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			return MoveJipCaret(menu, PrevCharBoundary(current, caret));
		}

		bool MoveJipCaretNext(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			return MoveJipCaret(menu, NextCharBoundary(current, JipCursorIndex(menu)));
		}

		bool MoveJipCaretLineStart(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return MoveJipCaret(menu, 0);

			const size_t lineStart = current.rfind('\n', caret ? caret - 1 : 0);
			return MoveJipCaret(menu, lineStart == std::string::npos ? 0 : lineStart + 1);
		}

		bool MoveJipCaretLineEnd(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			const size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			const size_t lineEnd = current.find('\n', caret);
			return MoveJipCaret(menu, lineEnd == std::string::npos ? current.size() : lineEnd);
		}

		bool MoveJipCaretByChars(TextEditMenu* menu, int count)
		{
			const std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (count < 0)
			{
				for (int i = 0; i < -count; ++i)
					caret = PrevCharBoundary(current, caret);
			}
			else
			{
				for (int i = 0; i < count; ++i)
					caret = NextCharBoundary(current, caret);
			}

			return MoveJipCaret(menu, caret);
		}

		bool InsertWideTextJip(TextEditMenu* menu, std::wstring_view value)
		{
			std::string converted = WideToCurrentCodePage(value);
			if (converted.empty())
				return false;

			return InsertJipTextAtCaret(menu, converted);
		}

		bool JipInputCompositionControlShouldSuppress(UInt32 input)
		{
			if (!IsImeCompositionActive())
				return false;

			switch (input)
			{
			case kInputCode_Backspace:
			case kInputCode_Delete:
			case kInputCode_ArrowLeft:
			case kInputCode_ArrowRight:
			case kInputCode_Home:
			case kInputCode_End:
			case kInputCode_PageUp:
			case kInputCode_PageDown:
				return true;
			default:
				return false;
			}
		}

		bool __fastcall JipTextInputAdapterEx::Input(TextEditMenu* apMenu, void*, UInt32 aiInput)
		{
			if (!LooksLikeJipTextInputStorage(apMenu))
				return CallJipOriginalInput(apMenu, aiInput);

			const bool editActive = JipIsActiveFlag(apMenu) != 0;
			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (IsImeConsumingAscii())
				{
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_composition_ascii", apMenu, aiInput);
					return true;
				}

				if (!editActive)
					return CallJipOriginalInput(apMenu, aiInput);

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(aiInput)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_duplicate_wndproc_ascii", apMenu, aiInput);
					return true;
				}

				const char ch = static_cast<char>(aiInput);
				if (!InsertJipTextAtCaret(apMenu, std::string_view(&ch, 1)))
				{
					DebugLogJipState("JipTextInputAdapter::Input", "reject_ascii_insert", apMenu, aiInput);
					return false;
				}

				DebugLogJipState("JipTextInputAdapter::Input", "insert_ascii", apMenu, aiInput);
				return true;
			}

			if (aiInput > 0x7F && aiInput < kInputCode_Backspace)
			{
				DebugLogJipState("JipTextInputAdapter::Input", "swallow_high_byte", apMenu, aiInput);
				return true;
			}

			if (JipInputCompositionControlShouldSuppress(aiInput))
			{
				DebugLogJipState("JipTextInputAdapter::Input", "suppress_composition_control", apMenu, aiInput);
				return true;
			}

			if (!editActive)
				return CallJipOriginalInput(apMenu, aiInput);

			switch (aiInput)
			{
			case kInputCode_Backspace:
				DebugLogJipState("JipTextInputAdapter::Input", "delete_previous", apMenu, aiInput);
				return DeletePreviousJipChar(apMenu);
			case kInputCode_Delete:
				DebugLogJipState("JipTextInputAdapter::Input", "delete_next", apMenu, aiInput);
				return DeleteNextJipChar(apMenu);
			case kInputCode_ArrowLeft:
				DebugLogJipState("JipTextInputAdapter::Input", "move_left", apMenu, aiInput);
				return MoveJipCaretPrevious(apMenu);
			case kInputCode_ArrowRight:
				DebugLogJipState("JipTextInputAdapter::Input", "move_right", apMenu, aiInput);
				return MoveJipCaretNext(apMenu);
			case kInputCode_Home:
				DebugLogJipState("JipTextInputAdapter::Input", "move_home", apMenu, aiInput);
				return MoveJipCaretLineStart(apMenu);
			case kInputCode_End:
				DebugLogJipState("JipTextInputAdapter::Input", "move_end", apMenu, aiInput);
				return MoveJipCaretLineEnd(apMenu);
			case kInputCode_Enter:
				if (JipMiscFlags(apMenu) & kJipEnterAcceptsOkFlag)
				{
					DebugLogJipState("JipTextInputAdapter::Input", "pass_enter_to_jip", apMenu, aiInput);
					return CallJipOriginalInput(apMenu, aiInput);
				}
				DebugLogJipState("JipTextInputAdapter::Input", "insert_newline", apMenu, aiInput);
				return InsertJipTextAtCaret(apMenu, std::string_view("\n", 1));
			case kInputCode_PageUp:
				DebugLogJipState("JipTextInputAdapter::Input", "page_up", apMenu, aiInput);
				return MoveJipCaretByChars(apMenu, -5);
			case kInputCode_PageDown:
				DebugLogJipState("JipTextInputAdapter::Input", "page_down", apMenu, aiInput);
				return MoveJipCaretByChars(apMenu, 5);
			case kInputCode_ArrowUp:
			case kInputCode_ArrowDown:
				DebugLogJipState("JipTextInputAdapter::Input", "pass_vertical_arrow_to_jip", apMenu, aiInput);
				return CallJipOriginalInput(apMenu, aiInput);
			default:
				DebugLogJipState("JipTextInputAdapter::Input", "pass_original", apMenu, aiInput);
				return CallJipOriginalInput(apMenu, aiInput);
			}
		}

		bool CommitCandidate(TextEditState& state, const std::string& candidate, size_t caret)
		{
			if (!FitsTextEditConstraints(state, candidate))
				return false;

			state.SetText(candidate.c_str());
			SetCaret(state, ClampToPrevBoundary(candidate, caret));
			state.bClearOnNextType = false;
			return true;
		}

		bool CommitCandidate(TextEditMenu* menu, const std::string& candidate, size_t caret)
		{
			if (!menu || !CommitCandidate(menu->xEditState, candidate, caret))
				return false;

			menu->Refresh();
			return true;
		}

		bool InsertTextAtCaret(TextEditState& state, std::string_view text)
		{
			if (text.empty())
				return false;

			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);

			if (state.bClearOnNextType)
			{
				current.clear();
				caret = 0;
			}

			std::string candidate;
			candidate.reserve(current.size() + text.size());
			candidate.append(current, 0, caret);
			candidate.append(text.data(), text.size());
			candidate.append(current, caret, std::string::npos);

			return CommitCandidate(state, candidate, caret + text.size());
		}

		bool InsertTextAtCaret(TextEditMenu* menu, std::string_view text)
		{
			if (!menu)
				return false;

			if (!InsertTextAtCaret(menu->xEditState, text))
				return false;

			menu->Refresh();
			return true;
		}

		bool DeletePreviousChar(TextEditState& state)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(current, caret);
			current.erase(previous, caret - previous);
			return CommitCandidate(state, current, previous);
		}

		bool DeleteNextChar(TextEditState& state)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (caret >= current.size())
				return true;

			const size_t next = NextCharBoundary(current, caret);
			current.erase(caret, next - caret);
			return CommitCandidate(state, current, caret);
		}

		bool MoveCaretPrevious(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, PrevCharBoundary(current, ClampToPrevBoundary(current, state.iCaretByteOffset)));
			return true;
		}

		bool MoveCaretNext(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, NextCharBoundary(current, state.iCaretByteOffset));
			return true;
		}

		bool MoveCaretHome(TextEditState& state)
		{
			SetCaret(state, 0);
			return true;
		}

		bool MoveCaretEnd(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, current.size());
			return true;
		}

		bool RemovePreviousAsciiCompositionEcho(TextEditState& state, wchar_t compositionLead)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (!caret)
				return false;

			const size_t previous = PrevCharBoundary(current, caret);
			if (caret - previous != 1)
				return false;

			if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(current[previous]), compositionLead))
				return false;

			current.erase(previous, 1);
			return CommitCandidate(state, current, previous);
		}

		bool RemovePreviousJipAsciiCompositionEcho(TextEditMenu* menu, wchar_t compositionLead)
		{
			if (!LooksLikeJipTextInputStorage(menu))
				return false;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return false;

			const size_t previous = PrevCharBoundary(current, caret);
			if (caret - previous != 1)
				return false;

			if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(current[previous]), compositionLead))
				return false;

			current.erase(previous, 1);
			return CommitJipCandidate(menu, current, previous);
		}

		void TryRemoveCompositionEcho()
		{
			if (s_compositionEchoChecked || s_imeCandidateState.composition.empty())
				return;

			s_compositionEchoChecked = true;
			if (!IsConfiguredImeLayout(s_window))
				return;

			const wchar_t compositionLead = s_imeCandidateState.composition.front();
			if (TextEditMenu* menu = GetActiveTextEditMenu())
			{
				if (RemovePreviousAsciiCompositionEcho(menu->xEditState, compositionLead))
				{
					menu->Refresh();
					DebugLogState("IMECompositionEcho", "remove_ascii_echo", menu, static_cast<SInt32>(compositionLead));
				}
				return;
			}

			if (TextEditMenu* jipMenu = GetCurrentJipTextInputMenu())
			{
				if (RemovePreviousJipAsciiCompositionEcho(jipMenu, compositionLead))
					DebugLogJipState("IMECompositionEcho", "remove_ascii_echo", jipMenu, static_cast<UInt32>(compositionLead));
				return;
			}

			if (RemovePreviousStewieAsciiCompositionEcho(compositionLead))
				DebugLog("tnvse_multibyte_input_event: source=IMECompositionEcho action=remove_ascii_echo_stewie input=0x%08X", static_cast<UInt32>(compositionLead));
		}

		std::wstring GetImeCompositionString(HWND hwnd, DWORD index)
		{
			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return {};

			const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
			if (bytes <= 0)
			{
				ImmReleaseContext(hwnd, context);
				return {};
			}

			std::wstring value(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
			ImmGetCompositionStringW(context, index, value.data(), bytes);
			ImmReleaseContext(hwnd, context);
			return value;
		}

		bool HasImeCompositionString(HWND hwnd)
		{
			if (!hwnd)
				return false;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return false;

			const LONG compositionBytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
			ImmReleaseContext(hwnd, context);
			return compositionBytes > 0;
		}

		bool IsImeCompositionActive()
		{
			if (s_imeComposing)
				return true;

			if (!s_window)
				return false;

			return HasImeCompositionString(s_window);
		}

		bool IsImeConsumingAscii()
		{
			if (!s_window || !IsConfiguredImeLayout(s_window))
				return false;

			if (IsImeCompositionActive())
				return true;

			if (IsNativeImeAsciiGuardActive())
				return true;

			if (s_textInputSessionActive && HasOverlayInputTarget())
			{
				if (!s_imeCandidateState.composition.empty() || !s_imeCandidateState.candidates.empty())
					return true;

				if (s_imeCandidateState.imeOpen
					&& (s_imeCandidateState.conversionMode & IME_CMODE_NATIVE))
					return true;
			}

			HIMC context = ImmGetContext(s_window);
			if (!context)
				return false;

			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			const bool isOpen = ImmGetOpenStatus(context) != FALSE;
			const bool hasConversionStatus = ImmGetConversionStatus(
				context,
				&conversionMode,
				&sentenceMode) != FALSE;
			ImmReleaseContext(s_window, context);

			return isOpen && hasConversionStatus && (conversionMode & IME_CMODE_NATIVE);
		}

		std::string WideToCurrentCodePage(std::wstring_view value)
		{
			if (value.empty() || !g_usingWinEncoding)
				return {};

			BOOL usedDefaultChar = FALSE;
			const int length = WideCharToMultiByte(
				g_usingWinEncoding,
				WC_NO_BEST_FIT_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				nullptr,
				0,
				nullptr,
				&usedDefaultChar);
			if (length <= 0 || usedDefaultChar)
				return {};

			std::string converted(static_cast<size_t>(length), '\0');
			usedDefaultChar = FALSE;
			WideCharToMultiByte(
				g_usingWinEncoding,
				WC_NO_BEST_FIT_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				converted.data(),
				length,
				nullptr,
				&usedDefaultChar);
			if (usedDefaultChar)
				return {};

			return converted;
		}

		std::wstring GetCurrentImeName(HWND hwnd)
		{
			if (g_bMultibyteInputUseTSFCandidates && s_tsfCandidateSink)
			{
				std::wstring tsfName = s_tsfCandidateSink->GetCurrentInputMethodName();
				if (!tsfName.empty())
					return tsfName;
			}

			DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : GetCurrentThreadId();
			HKL layout = GetKeyboardLayout(threadId);
			if (layout)
			{
				wchar_t description[128] = {};
				if (ImmGetDescriptionW(layout, description, ARRAYSIZE(description)) > 0 && description[0])
					return description;
			}

			wchar_t layoutName[KL_NAMELENGTH] = {};
			if (GetKeyboardLayoutNameW(layoutName) && layoutName[0])
				return layoutName;

			return L"IME";
		}

		void RefreshImeStatus(HWND hwnd, HKL expectedLayout = nullptr)
		{
			s_imeCandidateState.imeName = GetCurrentImeName(hwnd);
			s_imeCandidateState.imeOpen = false;
			s_imeCandidateState.conversionMode = 0;
			s_imeCandidateState.sentenceMode = 0;

			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
				return;

			HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
			if (!context)
				return;

			s_imeCandidateState.imeOpen = ImmGetOpenStatus(context) != FALSE;
			ImmGetConversionStatus(
				context,
				&s_imeCandidateState.conversionMode,
				&s_imeCandidateState.sentenceMode);
			ImmReleaseContext(hwnd, context);
		}

		void RefreshImeComposition(HWND hwnd)
		{
			s_imeCandidateState.composition = GetImeCompositionString(hwnd, GCS_COMPSTR);
		}

		void ClearImeCandidates()
		{
			s_imeCandidateState.candidates.clear();
			s_imeCandidateState.selection = 0;
			s_imeCandidateState.pageStart = 0;
			s_imeCandidateState.pageSize = 0;
			s_imeCandidateState.candidatesFromTsf = false;
			s_tsfCandidateActive = false;
		}

		void RefreshImeCandidatesFromImm(HWND hwnd)
		{
			ClearImeCandidates();

			HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
			if (!context)
				return;

			const DWORD bytes = ImmGetCandidateListW(context, 0, nullptr, 0);
			if (!bytes)
			{
				ImmReleaseContext(hwnd, context);
				return;
			}

			std::unique_ptr<char[]> buffer(new char[bytes]);
			auto* list = reinterpret_cast<LPCANDIDATELIST>(buffer.get());
			if (ImmGetCandidateListW(context, 0, list, bytes) != bytes)
			{
				ImmReleaseContext(hwnd, context);
				return;
			}

			if (list->dwStyle != IME_CAND_CODE)
			{
				s_imeCandidateState.selection = list->dwSelection;
				s_imeCandidateState.pageStart = list->dwPageStart;
				s_imeCandidateState.pageSize = list->dwPageSize;

				const DWORD pageEnd = std::min<DWORD>(
					list->dwCount,
					list->dwPageStart + std::min<DWORD>(list->dwPageSize, kMaxImeCandidatesToDisplay));
				for (DWORD index = list->dwPageStart; index < pageEnd; ++index)
				{
					const DWORD offset = list->dwOffset[index];
					if (!offset || offset >= bytes)
						continue;

					const wchar_t* candidate = reinterpret_cast<const wchar_t*>(buffer.get() + offset);
					if (candidate && *candidate)
						s_imeCandidateState.candidates.emplace_back(candidate);
				}
			}

			ImmReleaseContext(hwnd, context);
		}

		void RefreshImeCandidates(HWND hwnd)
		{
			if (!IsConfiguredImeLayout(hwnd))
			{
				ClearImeCandidates();
				return;
			}

			if (g_bMultibyteInputUseTSFCandidates
				&& s_tsfCandidateActive
				&& s_imeCandidateState.candidatesFromTsf
				&& !s_imeCandidateState.candidates.empty())
				return;

			RefreshImeCandidatesFromImm(hwnd);
		}

		void ClearImePreviewState()
		{
			s_imeCandidateState.composing = false;
			s_imeCandidateState.composition.clear();
			s_compositionEchoChecked = false;
			ClearImeCandidates();
		}

		void HideSystemImeWindows(HWND hwnd)
		{
			if (!g_bMultibyteInputHideSystemCandidateWindow || !hwnd)
				return;

			if (s_hidingSystemImeWindows)
				return;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			s_hidingSystemImeWindows = true;

			COMPOSITIONFORM compositionForm = {};
			compositionForm.dwStyle = CFS_FORCE_POSITION;
			compositionForm.ptCurrentPos.x = -32000;
			compositionForm.ptCurrentPos.y = -32000;
			ImmSetCompositionWindow(context, &compositionForm);

			for (DWORD i = 0; i < 4; ++i)
			{
				CANDIDATEFORM candidateForm = {};
				candidateForm.dwIndex = i;
				candidateForm.dwStyle = CFS_CANDIDATEPOS;
				candidateForm.ptCurrentPos.x = -32000;
				candidateForm.ptCurrentPos.y = -32000;
				ImmSetCandidateWindow(context, &candidateForm);
			}

			ImmReleaseContext(hwnd, context);
			s_hidingSystemImeWindows = false;
		}

		void CancelGameImeComposition(HWND hwnd)
		{
			if (!hwnd)
				return;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
			ImmReleaseContext(hwnd, context);
		}

		void EnsureConfiguredImeOpen(HWND hwnd, const char* reason, HKL expectedLayout)
		{
			if (!hwnd)
				return;

			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
			{
				s_nativeImeAsciiGuardUntilTick = 0;
				return;
			}

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			const bool wasOpen = ImmGetOpenStatus(context) != FALSE;
			const bool hasConversionStatus = ImmGetConversionStatus(
				context,
				&conversionMode,
				&sentenceMode) != FALSE;

			bool changed = false;
			if (!wasOpen)
			{
				ImmSetOpenStatus(context, TRUE);
				changed = true;
			}

			if (hasConversionStatus && !(conversionMode & IME_CMODE_NATIVE))
			{
				ImmSetConversionStatus(context, conversionMode | IME_CMODE_NATIVE, sentenceMode);
				conversionMode |= IME_CMODE_NATIVE;
				changed = true;
			}

			ImmReleaseContext(hwnd, context);

			const bool guardNativeAscii = hasConversionStatus && (conversionMode & IME_CMODE_NATIVE);
			if (guardNativeAscii)
				s_nativeImeAsciiGuardUntilTick = GetTickCount() + kNativeImeAsciiGuardMs;

			if (changed || guardNativeAscii)
			{
				RefreshImeStatus(hwnd, expectedLayout);
				DebugLog(
					"tnvse_multibyte_input: prepared configured IME reason=%s changed=%u guard=%u open=%u native=%u",
					reason ? reason : "unknown",
					changed ? 1 : 0,
					guardNativeAscii ? 1 : 0,
					s_imeCandidateState.imeOpen ? 1 : 0,
					(s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
			}
		}

		void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout)
		{
			if (!hwnd)
				return;

			ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
			s_gameImeContextDetached = false;
			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
			{
				s_nativeImeAsciiGuardUntilTick = 0;
				s_imeComposing = false;
				ClearImePreviewState();
			}
			EnsureConfiguredImeOpen(hwnd, reason, expectedLayout);
			RefreshImeStatus(hwnd, expectedLayout);
			DebugLog(
				"tnvse_multibyte_input: game IME default context enabled reason=%s open=%u native=%u",
				reason ? reason : "unknown",
				s_imeCandidateState.imeOpen ? 1 : 0,
				(s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
		}

		void SetGameImeEnabled(HWND hwnd, bool enable)
		{
			if (!hwnd)
				return;

			if (enable)
			{
				if (!s_gameImeContextDetached)
					return;

				RestoreDefaultGameImeContext(hwnd, "enable");
				DebugLog("tnvse_multibyte_input: game IME context enabled");
				return;
			}

			if (s_gameImeContextDetached)
				return;

			CancelGameImeComposition(hwnd);
			s_imeComposing = false;
			ClearImePreviewState();
			HideCandidateOverlay();

			if (!s_gameImeContextDetached)
			{
				ImmAssociateContext(hwnd, nullptr);
				s_gameImeContextDetached = true;
			}

			DebugLog("tnvse_multibyte_input: game IME context disabled");
		}

		void SetTextInputSessionActive(bool active)
		{
			if (s_textInputSessionActive == active)
				return;

			s_textInputSessionActive = active;
			if (s_window)
			{
				if (active)
					RestoreDefaultGameImeContext(s_window, "session_start");
				else
					SetGameImeEnabled(s_window, false);
			}

			DebugLog(
				"tnvse_multibyte_input: text input session %s",
				active ? "started" : "ended");
		}

		void UpdateGameImeAssociation()
		{
			if (!s_window)
				return;

			SetTextInputSessionActive(GetCurrentTextEditMenuObject() != nullptr || GetOverlayStewieInputTarget().valid);
		}

		bool IsImeWindowMessage(UINT msg)
		{
			switch (msg)
			{
			case WM_IME_STARTCOMPOSITION:
			case WM_IME_COMPOSITION:
			case WM_IME_ENDCOMPOSITION:
			case WM_IME_NOTIFY:
			case WM_IME_SETCONTEXT:
			case WM_IME_CHAR:
				return true;
			default:
				return false;
			}
		}

		bool IsVirtualKeyDown(int vk)
		{
			return (GetKeyState(vk) & 0x8000) != 0 || (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		bool IsWinSpaceInputLanguageHotkey(UINT msg, WPARAM wParam)
		{
			if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
				return false;

			if (wParam != VK_SPACE)
				return false;

			return IsVirtualKeyDown(VK_LWIN) || IsVirtualKeyDown(VK_RWIN);
		}

		HKL GetGameKeyboardLayout(HWND hwnd)
		{
			DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : GetCurrentThreadId();
			return GetKeyboardLayout(threadId);
		}

		bool LayoutMatchesCurrentEncoding(HKL layout)
		{
			if (!layout || !g_uiEncoding)
				return false;

			const LANGID language = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
			switch (g_uiEncoding)
			{
			case 1:
			case 2:
				return PRIMARYLANGID(language) == LANG_CHINESE;
			case 3:
				return PRIMARYLANGID(language) == LANG_JAPANESE;
			case 4:
				return PRIMARYLANGID(language) == LANG_KOREAN;
			default:
				return false;
			}
		}

		bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout)
		{
			HKL layout = expectedLayout ? expectedLayout : GetGameKeyboardLayout(hwnd);
			return LayoutMatchesCurrentEncoding(layout);
		}

		bool IsNativeImeAsciiGuardActive()
		{
			return s_window
				&& s_textInputSessionActive
				&& IsConfiguredImeLayout(s_window)
				&& static_cast<SInt32>(s_nativeImeAsciiGuardUntilTick - GetTickCount()) > 0;
		}

		bool IsFocusRestoreMessage(UINT msg, WPARAM wParam)
		{
			switch (msg)
			{
			case WM_SETFOCUS:
				return true;
			case WM_ACTIVATEAPP:
				return wParam != FALSE;
			case WM_ACTIVATE:
				return LOWORD(wParam) != WA_INACTIVE;
			default:
				return false;
			}
		}

		const wchar_t* GetNativeModeLabel()
		{
			const bool native = (s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) != 0;
			switch (g_uiEncoding)
			{
			case 3:
				return native ? L"\u65E5\u672C\u8A9E" : L"\u82F1\u6570";
			case 4:
				return native ? L"\uD55C\uAD6D\uC5B4" : L"\uC601\uBB38";
			default:
				return native ? L"\u4E2D\u6587" : L"\u82F1\u6587";
			}
		}

		std::wstring BuildImeStatusLineWide()
		{
			std::wstring line = s_imeCandidateState.imeName.empty()
				? L"IME"
				: s_imeCandidateState.imeName;
			line += s_imeCandidateState.imeOpen ? L" ON" : L" OFF";
			if (s_imeCandidateState.imeOpen)
			{
				line += L" ";
				line += GetNativeModeLabel();
				line += (s_imeCandidateState.conversionMode & IME_CMODE_FULLSHAPE)
					? L" \u5168\u89D2"
					: L" \u534A\u89D2";
			}
			return line;
		}

		std::vector<CandidateOverlayLine> BuildCandidateOverlayLines()
		{
			std::vector<CandidateOverlayLine> lines;
			if (!g_bMultibyteInputCompositionPreview || !HasOverlayInputTarget() || !s_imeCandidateState.imeOpen)
				return lines;

			lines.push_back({ BuildImeStatusLineWide(), false });

			if (!s_imeCandidateState.composition.empty())
			{
				std::wstring composition = L"> ";
				composition += s_imeCandidateState.composition;
				lines.push_back({ std::move(composition), false });
			}

			for (size_t i = 0; i < s_imeCandidateState.candidates.size(); ++i)
			{
				if (s_imeCandidateState.candidates[i].empty())
					continue;

				const DWORD globalIndex = s_imeCandidateState.pageStart + static_cast<DWORD>(i);
				wchar_t prefix[8] = {};
				std::swprintf(prefix, ARRAYSIZE(prefix), L"%u. ", static_cast<UInt32>(i + 1));
				std::wstring line = prefix;
				line += s_imeCandidateState.candidates[i];
				lines.push_back({ std::move(line), globalIndex == s_imeCandidateState.selection });
			}

			return lines;
		}

		std::wstring BuildCandidateOverlayKey(const std::vector<CandidateOverlayLine>& lines)
		{
			std::wstring key;
			for (const CandidateOverlayLine& line : lines)
			{
				key += line.highlighted ? L"\x0001" : L"\x0000";
				key += line.text;
				key += L"\n";
			}
			return key;
		}

		const wchar_t* GetOverlayFontName()
		{
			switch (g_uiEncoding)
			{
			case 2:
				return L"Microsoft JhengHei UI";
			case 3:
				return L"Meiryo";
			case 4:
				return L"Malgun Gothic";
			default:
				return L"Microsoft YaHei UI";
			}
		}

		void ReleaseCandidateOverlayTexture()
		{
			if (s_candidateOverlay.texture)
			{
				s_candidateOverlay.texture->Release();
				s_candidateOverlay.texture = nullptr;
			}
			s_candidateOverlay.textureWidth = 0;
			s_candidateOverlay.textureHeight = 0;
		}

		void HideCandidateOverlay()
		{
			s_candidateOverlay.visible = false;
			s_candidateOverlay.dirty = true;
			s_candidateOverlay.lastKey.clear();
		}

		void UpdateCandidateOverlay()
		{
			if (!g_bMultibyteInputCompositionPreview)
			{
				HideCandidateOverlay();
				return;
			}

			if (!HasOverlayInputTarget() || !s_imeCandidateState.imeOpen)
			{
				HideCandidateOverlay();
				return;
			}

			s_candidateOverlay.visible = true;
			s_candidateOverlay.dirty = true;
		}

		bool RenderOverlayTexture(
			LPDIRECT3DDEVICE9 device,
			const std::vector<CandidateOverlayLine>& lines)
		{
			if (!device || lines.empty())
				return false;

			HDC hdc = CreateCompatibleDC(nullptr);
			if (!hdc)
				return false;

			HFONT font = CreateFontW(
				-18,
				0,
				0,
				0,
				FW_NORMAL,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE,
				GetOverlayFontName());
			HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;

			UInt32 maxLineWidth = 0;
			for (const CandidateOverlayLine& line : lines)
			{
				SIZE current = {};
				if (GetTextExtentPoint32W(hdc, line.text.c_str(), static_cast<int>(line.text.size()), &current))
					maxLineWidth = std::max<UInt32>(maxLineWidth, static_cast<UInt32>(current.cx));
			}

			UInt32 width = std::clamp<UInt32>(maxLineWidth + kOverlayPadding * 2, kOverlayMinWidth, kOverlayMaxWidth);
			UInt32 height = kOverlayPadding * 2 + static_cast<UInt32>(lines.size()) * kOverlayLineHeight;
			width = std::max<UInt32>(width, 1);
			height = std::max<UInt32>(height, 1);

			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = static_cast<LONG>(width);
			bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			void* pixels = nullptr;
			HBITMAP bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
			if (!bitmap || !pixels)
			{
				if (oldFont)
					SelectObject(hdc, oldFont);
				if (font)
					DeleteObject(font);
				DeleteDC(hdc);
				return false;
			}

			HGDIOBJ oldBitmap = SelectObject(hdc, bitmap);
			RECT background = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
			HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 18, 18));
			FillRect(hdc, &background, backgroundBrush);
			DeleteObject(backgroundBrush);

			HBRUSH borderBrush = CreateSolidBrush(RGB(220, 220, 220));
			FrameRect(hdc, &background, borderBrush);
			DeleteObject(borderBrush);

			SetBkMode(hdc, TRANSPARENT);
			for (size_t i = 0; i < lines.size(); ++i)
			{
				RECT lineRect = {
					static_cast<LONG>(kOverlayPadding),
					static_cast<LONG>(kOverlayPadding + i * kOverlayLineHeight),
					static_cast<LONG>(width - kOverlayPadding),
					static_cast<LONG>(kOverlayPadding + (i + 1) * kOverlayLineHeight)
				};

				if (lines[i].highlighted)
				{
					RECT highlightRect = lineRect;
					highlightRect.left -= 4;
					highlightRect.right += 4;
					HBRUSH highlightBrush = CreateSolidBrush(RGB(58, 84, 126));
					FillRect(hdc, &highlightRect, highlightBrush);
					DeleteObject(highlightBrush);
				}

				SetTextColor(hdc, lines[i].highlighted ? RGB(255, 255, 255) : RGB(230, 230, 230));
				DrawTextW(
					hdc,
					lines[i].text.c_str(),
					static_cast<int>(lines[i].text.size()),
					&lineRect,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			}

			auto* argb = static_cast<UInt32*>(pixels);
			for (UInt32 i = 0; i < width * height; ++i)
				argb[i] |= 0xE8000000;

			if (!s_candidateOverlay.texture
				|| s_candidateOverlay.textureWidth != width
				|| s_candidateOverlay.textureHeight != height)
			{
				ReleaseCandidateOverlayTexture();
				if (FAILED(device->CreateTexture(
					width,
					height,
					1,
					0,
					D3DFMT_A8R8G8B8,
					D3DPOOL_MANAGED,
					&s_candidateOverlay.texture,
					nullptr)))
				{
					SelectObject(hdc, oldBitmap);
					DeleteObject(bitmap);
					if (oldFont)
						SelectObject(hdc, oldFont);
					if (font)
						DeleteObject(font);
					DeleteDC(hdc);
					return false;
				}

				s_candidateOverlay.textureWidth = width;
				s_candidateOverlay.textureHeight = height;
			}

			D3DLOCKED_RECT locked = {};
			if (SUCCEEDED(s_candidateOverlay.texture->LockRect(0, &locked, nullptr, 0)))
			{
				for (UInt32 y = 0; y < height; ++y)
				{
					std::memcpy(
						static_cast<UInt8*>(locked.pBits) + y * locked.Pitch,
						argb + y * width,
						width * sizeof(UInt32));
				}
				s_candidateOverlay.texture->UnlockRect(0);
			}

			SelectObject(hdc, oldBitmap);
			DeleteObject(bitmap);
			if (oldFont)
				SelectObject(hdc, oldFont);
			if (font)
				DeleteObject(font);
			DeleteDC(hdc);
			return true;
		}

		struct OverlayVertex
		{
			float x;
			float y;
			float z;
			float rhw;
			D3DCOLOR color;
			float u;
			float v;
		};

		void DrawCandidateOverlay()
		{
			if (!g_bMultibyteInputCompositionPreview || !s_candidateOverlay.visible)
				return;

			std::vector<CandidateOverlayLine> lines = BuildCandidateOverlayLines();
			if (lines.empty())
			{
				HideCandidateOverlay();
				return;
			}

			std::wstring key = BuildCandidateOverlayKey(lines);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			LPDIRECT3DDEVICE9 device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return;

			if (s_candidateOverlay.dirty || key != s_candidateOverlay.lastKey || !s_candidateOverlay.texture)
			{
				if (!RenderOverlayTexture(device, lines))
					return;

				s_candidateOverlay.lastKey = std::move(key);
				s_candidateOverlay.dirty = false;
			}

			if (!s_candidateOverlay.texture)
				return;

			D3DVIEWPORT9 viewport = {};
			if (FAILED(device->GetViewport(&viewport)))
				return;

			const float x = std::max<float>(
				12.0f,
				(static_cast<float>(viewport.Width) - static_cast<float>(s_candidateOverlay.textureWidth)) * 0.5f);
			const float y = std::min<float>(
				static_cast<float>(viewport.Height) - static_cast<float>(s_candidateOverlay.textureHeight) - 12.0f,
				static_cast<float>(viewport.Height) * 0.58f);
			const float right = x + static_cast<float>(s_candidateOverlay.textureWidth);
			const float bottom = y + static_cast<float>(s_candidateOverlay.textureHeight);

			OverlayVertex vertices[4] = {
				{ x - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
				{ right - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
				{ x - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
				{ right - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
			};

			IDirect3DStateBlock9* stateBlock = nullptr;
			if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
				stateBlock->Capture();

			device->SetTexture(0, s_candidateOverlay.texture);
			device->SetPixelShader(nullptr);
			device->SetVertexShader(nullptr);
			device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
			device->SetRenderState(D3DRS_ZENABLE, FALSE);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			device->SetRenderState(D3DRS_LIGHTING, FALSE);
			device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(OverlayVertex));

			if (stateBlock)
			{
				stateBlock->Apply();
				stateBlock->Release();
			}
		}

		bool InsertWideText(TextEditMenu* menu, std::wstring_view value)
		{
			std::string converted = WideToCurrentCodePage(value);
			if (converted.empty())
				return false;

			return InsertTextAtCaret(menu, converted);
		}

		bool ValidatePlayerName(const char* text)
		{
			if (!text || !*text)
				return false;

			bool hasVisibleCharacter = false;
			const size_t length = std::strlen(text);

			for (size_t i = 0; i < length;)
			{
				const UInt8 ch = static_cast<UInt8>(text[i]);
				if (ch >= 0x80)
				{
					UInt32 code = 0;
					if (i + 1 >= length || !TryDecodeDoubleByte(&text[i], code))
						return false;

					hasVisibleCharacter = true;
					i += 2;
					continue;
				}

				UInt8 converted = ch;
				Font::ConvertCharacter(converted);
				if (converted == '\\' || converted == '~')
					return false;

				if (converted != ' ')
					hasVisibleCharacter = true;

				++i;
			}

			return hasVisibleCharacter;
		}

		bool HandleImeResult(HWND hwnd, LPARAM lParam)
		{
			if (!(lParam & GCS_RESULTSTR))
				return false;

			TextEditMenu* menu = GetActiveTextEditMenu();
			TextEditMenu* jipMenu = menu ? nullptr : GetActiveJipTextInputMenu();
			if (!menu && !jipMenu)
				jipMenu = GetCurrentJipTextInputMenu();
			StewieInputTarget stewieTarget = (!menu && !jipMenu) ? GetOverlayStewieInputTarget() : StewieInputTarget();
			if (!menu && !jipMenu && !stewieTarget.valid)
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_no_active_target", nullptr, static_cast<SInt32>(lParam));
				return false;
			}

			std::wstring result = GetImeCompositionString(hwnd, GCS_RESULTSTR);
			if (result.empty())
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_empty", jipMenu, static_cast<UInt32>(lParam));
				else if (stewieTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_empty_stewie input=0x%08X", static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_empty", menu, static_cast<SInt32>(lParam));
				return false;
			}

			const bool inserted = menu
				? InsertWideText(menu, result)
				: (jipMenu ? InsertWideTextJip(jipMenu, result) : InsertWideTextStewie(stewieTarget, result));
			if (!inserted)
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_rejected", jipMenu, static_cast<UInt32>(lParam));
				else if (stewieTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_rejected_stewie input=0x%08X", static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_rejected", menu, static_cast<SInt32>(lParam));
				DebugLog("tnvse_multibyte_input: rejected IME result length=%u", static_cast<UInt32>(result.size()));
				return false;
			}

			s_lastImeCommitTick = GetTickCount();
			s_suppressedImeCharCount = static_cast<UInt32>(result.size());
			ClearImePreviewState();
			RefreshImeStatus(hwnd);
			UpdateCandidateOverlay();
			if (jipMenu)
				DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_inserted", jipMenu, static_cast<UInt32>(lParam));
			else if (stewieTarget.valid)
				DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_inserted_stewie input=0x%08X", static_cast<UInt32>(lParam));
			else
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_inserted", menu, static_cast<SInt32>(lParam));
			DebugLog("tnvse_multibyte_input: committed IME result chars=%u", s_suppressedImeCharCount);
			return true;
		}

		bool ShouldSuppressDuplicateImeChar()
		{
			if (!s_suppressedImeCharCount)
				return false;

			if (GetTickCount() - s_lastImeCommitTick > kDuplicateImeCharSuppressMs)
			{
				s_suppressedImeCharCount = 0;
				return false;
			}

			--s_suppressedImeCharCount;
			return true;
		}

		bool HandleCharFallback(WPARAM wParam)
		{
			if (ShouldSuppressDuplicateImeChar())
			{
				if (TextEditMenu* jipMenu = GetActiveJipTextInputMenu())
					DebugLogJipState("WndProc.WM_CHAR", "suppress_duplicate_ime_char", jipMenu, static_cast<UInt32>(wParam));
				else
					DebugLogState("WndProc.WM_CHAR", "suppress_duplicate_ime_char", GetActiveTextEditMenu(), static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam > 0xFFFF)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_out_of_range", GetAnyActiveTextInputMenu(), static_cast<SInt32>(wParam));
				return false;
			}

			TextEditMenu* menu = GetActiveTextEditMenu();
			TextEditMenu* jipMenu = menu ? nullptr : GetActiveJipTextInputMenu();
			StewieInputTarget stewieTarget = (!menu && !jipMenu) ? GetActiveStewieInputTarget() : StewieInputTarget();
			if (!menu && !jipMenu)
			{
				if (stewieTarget.valid)
				{
					if (wParam >= 0x20 && wParam <= 0x7E)
					{
						if (IsImeConsumingAscii())
						{
							DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=suppress_composition_ascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
							return true;
						}

						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=consume_ascii_handled_by_stewie_adapter input=0x%08X", static_cast<UInt32>(wParam));
						return true;
					}

					if (wParam < 0x80)
						return false;

					const wchar_t ch = static_cast<wchar_t>(wParam);
					if (!InsertWideTextStewie(stewieTarget, std::wstring_view(&ch, 1)))
						return false;

					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=insert_nonascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
					return true;
				}

				if (wParam >= 0x20 && wParam <= 0x7E && HasOverlayInputTarget() && IsImeConsumingAscii())
				{
					DebugLogState("WndProc.WM_CHAR", "suppress_overlay_composition_ascii", GetOverlayTextInputMenu(), static_cast<SInt32>(wParam));
					return true;
				}

				DebugLogState("WndProc.WM_CHAR", "pass_no_active_target", nullptr, static_cast<SInt32>(wParam));
				return false;
			}

			if (jipMenu)
			{
				if (wParam >= 0x20 && wParam <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						DebugLogJipState("WndProc.WM_CHAR", "suppress_composition_ascii", jipMenu, static_cast<UInt32>(wParam));
						return true;
					}

					DebugLogJipState("WndProc.WM_CHAR", "consume_ascii_handled_by_jip_adapter", jipMenu, static_cast<UInt32>(wParam));
					return true;
				}

				if (wParam < 0x80)
				{
					DebugLogJipState("WndProc.WM_CHAR", "pass_control_char", jipMenu, static_cast<UInt32>(wParam));
					return false;
				}

				const wchar_t ch = static_cast<wchar_t>(wParam);
				if (!InsertWideTextJip(jipMenu, std::wstring_view(&ch, 1)))
				{
					DebugLogJipState("WndProc.WM_CHAR", "reject_nonascii_insert", jipMenu, static_cast<UInt32>(wParam));
					return false;
				}

				DebugLogJipState("WndProc.WM_CHAR", "insert_nonascii", jipMenu, static_cast<UInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E && IsImeConsumingAscii())
			{
				DebugLogState("WndProc.WM_CHAR", "suppress_composition_ascii", menu, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E)
			{
				DebugLogState("WndProc.WM_CHAR", "consume_ascii_handled_by_textedit", menu, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam < 0x80)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_control_char", menu, static_cast<SInt32>(wParam));
				return false;
			}

			const wchar_t ch = static_cast<wchar_t>(wParam);
			if (!InsertWideText(menu, std::wstring_view(&ch, 1)))
			{
				DebugLogState("WndProc.WM_CHAR", "reject_nonascii_insert", menu, static_cast<SInt32>(wParam));
				return false;
			}

			DebugLogState("WndProc.WM_CHAR", "insert_nonascii", menu, static_cast<SInt32>(wParam));
			return true;
		}

		LRESULT CALLBACK MultibyteInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			if (s_hooksInstalled)
			{
				TryInstallJipTextInputHook();
				TryInstallStewieTweaksInputHooks();
				TextEditMenu* inputTarget = GetOverlayTextInputMenu();
				StewieInputTarget stewieOverlayTarget = GetOverlayStewieInputTarget();
				const bool hasInputTarget = inputTarget || stewieOverlayTarget.valid;
				if (hasInputTarget)
				{
					SetTextInputSessionActive(true);
				}
				else if (s_textInputSessionActive && GetCurrentTextEditMenuObject() == nullptr)
				{
					SetTextInputSessionActive(false);
				}

				if (msg == WM_INPUTLANGCHANGEREQUEST && s_textInputSessionActive)
				{
					ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
					s_gameImeContextDetached = false;
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_INPUTLANGCHANGEREQUEST action=def_window_proc");
					return DefWindowProcA(hwnd, msg, wParam, lParam);
				}

				if (msg == WM_INPUTLANGCHANGE && s_textInputSessionActive)
				{
					HKL newLayout = reinterpret_cast<HKL>(lParam);
					RestoreDefaultGameImeContext(hwnd, "inputlangchange", newLayout);
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_INPUTLANGCHANGE action=refresh_ime_name layout=0x%08X configured=%u",
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(newLayout)),
						LayoutMatchesCurrentEncoding(newLayout) ? 1 : 0);
					return 0;
				}

				if (s_textInputSessionActive && IsWinSpaceInputLanguageHotkey(msg, wParam))
				{
					RestoreDefaultGameImeContext(hwnd, "winspace_before");
					HKL previousLayout = ActivateKeyboardLayout(reinterpret_cast<HKL>(HKL_NEXT), KLF_SETFORPROCESS);
					HKL currentLayout = GetGameKeyboardLayout(hwnd);
					RestoreDefaultGameImeContext(hwnd, "winspace_after", currentLayout);
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc action=winspace_next_layout previous=0x%08X current=0x%08X",
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(previousLayout)),
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(currentLayout)));
					return 0;
				}

				if (s_textInputSessionActive && IsFocusRestoreMessage(msg, wParam))
				{
					RestoreDefaultGameImeContext(hwnd, "focus_restore");
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLog("tnvse_multibyte_input_event: source=WndProc action=focus_restore_ime msg=0x%04X", static_cast<UInt32>(msg));
				}

				if (hasInputTarget)
				{
					SetGameImeEnabled(hwnd, true);
				}
				else if (IsImeWindowMessage(msg))
				{
					SetGameImeEnabled(hwnd, false);
					DebugLog("tnvse_multibyte_input_event: source=WndProc action=suppress_ime_without_target msg=0x%04X", static_cast<UInt32>(msg));
					if (msg == WM_IME_SETCONTEXT)
						return DefWindowProcA(hwnd, WM_IME_SETCONTEXT, wParam, 0);
					return 0;
				}

				if (msg == WM_IME_STARTCOMPOSITION && hasInputTarget)
				{
					HideSystemImeWindows(hwnd);
					s_imeComposing = true;
					s_compositionEchoChecked = false;
					s_imeCandidateState.composing = true;
					RefreshImeStatus(hwnd);
					RefreshImeComposition(hwnd);
					TryRemoveCompositionEcho();
					RefreshImeCandidates(hwnd);
					UpdateCandidateOverlay();
					DebugLogState("WndProc.WM_IME_STARTCOMPOSITION", "composition_start", GetAnyActiveTextInputMenu(), 0);
				}

				if (msg == WM_IME_NOTIFY && hasInputTarget)
				{
					RefreshImeStatus(hwnd);
					switch (wParam)
					{
					case IMN_OPENCANDIDATE:
					case IMN_SETCANDIDATEPOS:
					case IMN_CHANGECANDIDATE:
						RefreshImeCandidates(hwnd);
						UpdateCandidateOverlay();
						DebugLog(
							"tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=refresh_candidates notify=0x%08X count=%u selection=%u pageStart=%u pageSize=%u",
							static_cast<UInt32>(wParam),
							static_cast<UInt32>(s_imeCandidateState.candidates.size()),
							static_cast<UInt32>(s_imeCandidateState.selection),
							static_cast<UInt32>(s_imeCandidateState.pageStart),
							static_cast<UInt32>(s_imeCandidateState.pageSize));
						return 0;
					case IMN_CLOSECANDIDATE:
						ClearImeCandidates();
						UpdateCandidateOverlay();
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=close_candidates");
						return 0;
					case IMN_SETOPENSTATUS:
					case IMN_SETCONVERSIONMODE:
					case IMN_SETSENTENCEMODE:
						UpdateCandidateOverlay();
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=refresh_ime_status");
						break;
					default:
						break;
					}
				}

				if (msg == WM_IME_COMPOSITION)
				{
					TextEditMenu* activeTarget = GetAnyActiveTextInputMenu();
					TextEditMenu* overlayTarget = inputTarget;
					if (hasInputTarget)
						HideSystemImeWindows(hwnd);
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION lParam=0x%08X hasResult=%u hasComp=%u composingBefore=%u active=0x%08X overlay=0x%08X stewie=%u",
						static_cast<UInt32>(lParam),
						(lParam & GCS_RESULTSTR) ? 1 : 0,
						(lParam & GCS_COMPSTR) ? 1 : 0,
						s_imeComposing ? 1 : 0,
						reinterpret_cast<UInt32>(activeTarget),
						reinterpret_cast<UInt32>(overlayTarget),
						stewieOverlayTarget.valid ? 1 : 0);

					if (HandleImeResult(hwnd, lParam))
					{
						s_imeComposing = false;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_result_consumed", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}

					if (hasInputTarget)
					{
						if (lParam & GCS_RESULTSTR)
						{
							s_imeComposing = false;
							ClearImePreviewState();
						}
						else
						{
							s_imeComposing = true;
							s_imeCandidateState.composing = true;
						}
						RefreshImeStatus(hwnd);
						if (lParam & GCS_COMPSTR)
						{
							RefreshImeComposition(hwnd);
							TryRemoveCompositionEcho();
						}
						RefreshImeCandidates(hwnd);
						UpdateCandidateOverlay();
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_continue", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}
				}

				if (msg == WM_IME_ENDCOMPOSITION)
				{
					s_imeComposing = false;
					ClearImePreviewState();
					RefreshImeStatus(hwnd);
					UpdateCandidateOverlay();
					DebugLogState("WndProc.WM_IME_ENDCOMPOSITION", "composition_end", GetAnyActiveTextInputMenu(), 0);
					if (hasInputTarget)
						return 0;
				}

				if (msg == WM_IME_SETCONTEXT
					&& g_bMultibyteInputHideSystemCandidateWindow
					&& hasInputTarget)
				{
					HideSystemImeWindows(hwnd);
					return DefWindowProcA(hwnd, WM_IME_SETCONTEXT, wParam, 0);
				}

				if (msg == WM_IME_CHAR && hasInputTarget)
				{
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_CHAR action=suppress_ime_char input=0x%08X", static_cast<UInt32>(wParam));
					return 0;
				}

				if (msg == WM_CHAR && HandleCharFallback(wParam))
					return 0;
			}

			if (msg == WM_NCDESTROY && hwnd == s_window && s_originalWndProc)
			{
				WNDPROC original = s_originalWndProc;
				SetGameImeEnabled(hwnd, true);
				SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
				s_originalWndProc = nullptr;
				s_window = nullptr;
				return CallWindowProcA(original, hwnd, msg, wParam, lParam);
			}

			return CallWindowProcA(s_originalWndProc, hwnd, msg, wParam, lParam);
		}

		struct WindowSearch
		{
			DWORD processId = 0;
			HWND window = nullptr;
		};

		BOOL CALLBACK EnumProcessWindows(HWND hwnd, LPARAM lParam)
		{
			auto* search = reinterpret_cast<WindowSearch*>(lParam);
			DWORD processId = 0;
			GetWindowThreadProcessId(hwnd, &processId);
			if (processId != search->processId)
				return TRUE;

			if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
				return TRUE;

			char className[64] = {};
			GetClassNameA(hwnd, className, sizeof(className));
			if (!_stricmp(className, "ConsoleWindowClass"))
				return TRUE;

			search->window = hwnd;
			return FALSE;
		}

		HWND FindGameWindow()
		{
			const DWORD currentProcessId = GetCurrentProcessId();
			HWND foreground = GetForegroundWindow();
			if (foreground)
			{
				DWORD foregroundProcessId = 0;
				GetWindowThreadProcessId(foreground, &foregroundProcessId);
				if (foregroundProcessId == currentProcessId && IsWindowVisible(foreground))
					return foreground;
			}

			WindowSearch search;
			search.processId = currentProcessId;
			EnumWindows(EnumProcessWindows, reinterpret_cast<LPARAM>(&search));
			return search.window;
		}

		bool TryInstallWindowProc()
		{
			if (s_originalWndProc)
				return true;

			HWND hwnd = FindGameWindow();
			if (!hwnd)
				return false;

			LONG_PTR original = SetWindowLongPtrA(
				hwnd,
				GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(&MultibyteInputWndProc));
			if (!original)
				return false;

			s_window = hwnd;
			s_originalWndProc = reinterpret_cast<WNDPROC>(original);
			SetGameImeEnabled(hwnd, false);
			DebugLog("tnvse_multibyte_input: subclassed hwnd=0x%08X", reinterpret_cast<UInt32>(hwnd));
			return true;
		}

		void ClearInputState()
		{
			s_textInputSessionActive = false;
			s_imeComposing = false;
			ClearImePreviewState();
			HideCandidateOverlay();
			ReleaseCandidateOverlayTexture();
			s_suppressedImeCharCount = 0;
			s_lastImeCommitTick = 0;
			s_lastWndProcAsciiTick = 0;
			s_lastWndProcAsciiChar = 0;
			ClearJipTextInputHookState();
			ClearStewieInputState();
			s_stewieReplay = false;
		}

		void RestoreWindowProc()
		{
			if (CurrentTextEditInputHandler() == JipTextInputHandlerAddress())
				SafeWrite32(kTextEditMenuInputVTableEntry, kTextEditMenuHandleKeyboardInput);

			if (s_window && s_originalWndProc)
			{
				SetGameImeEnabled(s_window, true);
				SetWindowLongPtrA(s_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
			}

			s_window = nullptr;
			s_originalWndProc = nullptr;
			if (s_tsfCandidateSink)
			{
				s_tsfCandidateSink->Shutdown();
				s_tsfCandidateSink.reset();
			}
			ClearInputState();
		}
	}

	class TextEditMenuEx : public TextEditMenu
	{
	public:
		static bool __cdecl Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText)
		{
			ValidateTextCallback validateText = apValidateText;
			if (reinterpret_cast<SIZE_T>(apValidateText) == kPlayerNameIsValidName)
				validateText = &ValidatePlayerName;

			const bool opened = TextEditMenu::Open(apTitle, apInitialText, validateText);
			if (opened && s_window)
				SetTextInputSessionActive(true);
			DebugLog(
				"tnvse_multibyte_input: TextEditMenu::Open opened=%u title=\"%s\" initialLen=%u menu=0x%08X",
				opened ? 1 : 0,
				apTitle ? apTitle : "",
				apInitialText ? static_cast<UInt32>(std::strlen(apInitialText)) : 0,
				reinterpret_cast<UInt32>(opened ? TextEditMenu::GetCurrent() : nullptr));
			return opened;
		}
	};

	class TextEditStateEx : public TextEditState
	{
	public:
		static void __fastcall Input(TextEditState* apState, void*, SInt32 aiInput, SInt32 aiChar)
		{
			if (!apState || !apState->IsActive())
				return;

			TextEditMenu* menu = GetActiveTextEditMenu();
			if (menu && &menu->xEditState != apState)
				menu = nullptr;

			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (IsImeConsumingAscii())
				{
					DebugLogState("TextEditState::Input", "suppress_composition_ascii", menu, aiInput);
					return;
				}

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(aiInput)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					DebugLogState("TextEditState::Input", "suppress_duplicate_wndproc_ascii", menu, aiInput);
					return;
				}

				const char ch = static_cast<char>(aiInput);
				if (!InsertTextAtCaret(*apState, std::string_view(&ch, 1)))
				{
					DebugLogState("TextEditState::Input", "reject_ascii_insert", menu, aiInput);
					return;
				}

				DebugLogState("TextEditState::Input", "insert_ascii", menu, aiInput);
				return;
			}

			if (aiInput > 0x7F && aiInput <= 0xFF)
			{
				DebugLogState("TextEditState::Input", "swallow_high_byte", menu, aiInput);
				return;
			}

			const bool imeCompositionActive = IsImeCompositionActive();
			switch (aiInput)
			{
			case kTextEditInput_Backspace:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "delete_previous", menu, aiInput);
				DeletePreviousChar(*apState);
				return;
			case kTextEditInput_Delete:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "delete_next", menu, aiInput);
				DeleteNextChar(*apState);
				return;
			case kTextEditInput_Left:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_left", menu, aiInput);
				MoveCaretPrevious(*apState);
				return;
			case kTextEditInput_Right:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_right", menu, aiInput);
				MoveCaretNext(*apState);
				return;
			case kTextEditInput_Home:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_home", menu, aiInput);
				MoveCaretHome(*apState);
				return;
			case kTextEditInput_End:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_end", menu, aiInput);
				MoveCaretEnd(*apState);
				return;
			default:
				DebugLogState("TextEditState::Input", "pass_original", menu, aiInput);
				apState->InputUnk01(aiInput, aiChar);
				return;
			}
		}
	};

	bool __fastcall StewieTweaksInputTargetEx::StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::InventoryMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::StatsMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::MapMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::ContainerMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::BarterMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::LevelUpMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::RecipeMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::StartMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	void InitMultibyteInputHook()
	{
		if (s_initialized)
			return;

		s_initialized = true;

		if (!g_bMultibyteInput)
			return;

		if (!g_bEnableMultibyteFontHook || !g_usingWinEncoding)
		{
			gLog.FormattedMessage("tnvse_multibyte_input: disabled because font hooks or uiEncoding are disabled");
			return;
		}

		WriteRelCall(kPlayerNameTextEditOpenCall, &TextEditMenuEx::Open);
		WriteRelCall(kTextEditStateInputCallInHandleKeyboardInput, &TextEditStateEx::Input);
		s_hooksInstalled = true;
		TryInstallWindowProc();
		if (g_bMultibyteInputCompositionPreview)
		{
			if (g_bMultibyteInputUseTSFCandidates)
			{
				s_tsfCandidateSink = std::make_unique<TsfCandidateSink>();
				const bool tsfInitialized = s_tsfCandidateSink->Initialize();
				if (!tsfInitialized)
				{
					s_tsfCandidateSink.reset();
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink unavailable; using IMM32 fallback");
				}
				else
				{
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink enabled");
				}
			}
			gLog.FormattedMessage("tnvse_multibyte_input: composition preview overlay enabled");
		}
		gLog.FormattedMessage("tnvse_multibyte_input: hooks installed");
	}

	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage)
	{
		if (!s_initialized || !g_bMultibyteInput || !apMessage)
			return;

		if (apMessage->type == kMessage_OnFramePresent)
		{
			if (s_hooksInstalled && s_window && g_bMultibyteInputCompositionPreview)
				DrawCandidateOverlay();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			if (s_hooksInstalled && !s_originalWndProc)
				TryInstallWindowProc();

			if (s_hooksInstalled)
				TryInstallJipTextInputHook();

			if (s_hooksInstalled)
				TryInstallStewieTweaksInputHooks();

			if (s_hooksInstalled && s_window)
				UpdateGameImeAssociation();

			if (s_hooksInstalled && s_window && g_bMultibyteInputCompositionPreview)
			{
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
			}
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame
			|| apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			RestoreWindowProc();
		}
	}
}
