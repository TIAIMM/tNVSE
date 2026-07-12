#include "multibyte_input_internal.h"

// TSF/IMM32 composition, candidate overlay rendering, and window-message routing.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr DWORD kDuplicateImeCharSuppressMs = 250;
		constexpr DWORD kNativeImeAsciiGuardMs = 250;
		constexpr UInt32 kMaxImeCandidatesToDisplay = 9;

		bool s_compositionEchoChecked = false;
		DWORD s_lastImeCommitTick = 0;
		UInt32 s_suppressedImeCharCount = 0;
		DWORD s_lastStewieImeCommitTick = 0;
		DWORD s_lastStewieImeEnterKeyTick = 0;
		DWORD s_inputLanguageSwitchGuardUntilTick = 0;

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
		bool s_gameImeEnabled = false;
		bool s_textInputSessionActive = false;
		DWORD s_nativeImeAsciiGuardUntilTick = 0;

		struct TsfUiElementSession
		{
			DWORD id = 0;
			UInt32 generation = 0;
		};

		UInt32 s_tsfSessionGeneration = 1;
		std::vector<TsfUiElementSession> s_tsfUiElementSessions;

		void AdvanceTsfCandidateSession()
		{
			if (++s_tsfSessionGeneration == 0)
			{
				s_tsfSessionGeneration = 1;
				s_tsfUiElementSessions.clear();
			}
		}

		bool RegisterTsfUiElement(DWORD id)
		{
			const auto existing = std::find_if(
				s_tsfUiElementSessions.begin(),
				s_tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing != s_tsfUiElementSessions.end())
				return existing->generation == s_tsfSessionGeneration;

			constexpr size_t kMaxRememberedTsfUiElements = 64;
			if (s_tsfUiElementSessions.size() >= kMaxRememberedTsfUiElements)
				s_tsfUiElementSessions.erase(s_tsfUiElementSessions.begin());
			s_tsfUiElementSessions.push_back({ id, s_tsfSessionGeneration });
			return true;
		}

		bool IsCurrentTsfUiElement(DWORD id)
		{
			return std::any_of(
				s_tsfUiElementSessions.begin(),
				s_tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id && value.generation == s_tsfSessionGeneration;
				});
		}

		bool ReleaseTsfUiElement(DWORD id)
		{
			const auto existing = std::find_if(
				s_tsfUiElementSessions.begin(),
				s_tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing == s_tsfUiElementSessions.end())
				return false;

			const bool wasCurrent = existing->generation == s_tsfSessionGeneration;
			s_tsfUiElementSessions.erase(existing);
			return wasCurrent;
		}

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
				const bool hasOverlayTarget = HasOverlayInputTarget();
				const bool acceptsCandidates = s_imeComposing
					&& hasOverlayTarget
					&& RegisterTsfUiElement(dwUIElementId);
				if (pbShow
					&& g_bMultibyteInputHideSystemCandidateWindow
					&& IsCandidateOverlayRendererAvailable()
					&& hasOverlayTarget)
					*pbShow = FALSE;

				if (!acceptsCandidates)
				{
					ClearImeCandidates();
					UpdateCandidateOverlay();
					return S_OK;
				}

				ReadCandidateElement(dwUIElementId);
				UpdateCandidateOverlay();
				return S_OK;
			}

			STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override
			{
				if (!s_imeComposing
					|| !HasOverlayInputTarget()
					|| !IsCurrentTsfUiElement(dwUIElementId))
				{
					ClearImeCandidates();
					UpdateCandidateOverlay();
					return S_OK;
				}

				ReadCandidateElement(dwUIElementId);
				UpdateCandidateOverlay();
				return S_OK;
			}

			STDMETHODIMP EndUIElement(DWORD dwUIElementId) override
			{
				if (!ReleaseTsfUiElement(dwUIElementId))
					return S_OK;

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

		bool InitializeTsfCandidateSupport()
		{
			if (s_tsfCandidateSink)
				return true;

			s_tsfCandidateSink = std::make_unique<TsfCandidateSink>();
			if (!s_tsfCandidateSink->Initialize())
			{
				s_tsfCandidateSink.reset();
				return false;
			}

			return true;
		}

		bool HasOverlayInputTarget()
		{
			return GetOverlayTextInputMenu() || GetOverlayStewieInputTarget().valid;
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
			}

			// Open/native is only the requested IMM compatibility state. Modern TSF
			// IMEs can report it before they actually start a composition. Treating it
			// as permanent consumption loses every ASCII key until the input profile is
			// switched. The short activation guard above covers the first-key race;
			// after that, only real composition or candidate state consumes ASCII.
			return false;
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

		void RefreshImeStatus(HWND hwnd, HKL expectedLayout)
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
			if (!g_bMultibyteInputHideSystemCandidateWindow
				|| !IsCandidateOverlayRendererAvailable()
				|| !hwnd)
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
			if (s_gameImeEnabled == enable)
				return;

			// Change the state before calling IMM. Cancel can synchronously deliver
			// IME messages, and those messages must observe the disabled state.
			s_gameImeEnabled = enable;

			if (enable)
			{
				RestoreDefaultGameImeContext(hwnd, "enable");
				DebugLog("tnvse_multibyte_input: game IME context enabled");
				return;
			}

			CancelGameImeComposition(hwnd);
			s_imeComposing = false;
			ClearImePreviewState();
			HideCandidateOverlay();
			DebugLog("tnvse_multibyte_input: game IME input disabled; context retained");
		}

		void SetTextInputSessionActive(bool active)
		{
			if (s_textInputSessionActive == active)
				return;

			s_textInputSessionActive = active;
			if (s_window)
				SetGameImeEnabled(s_window, active);

			DebugLog(
				"tnvse_multibyte_input: text input session %s",
				active ? "started" : "ended");
		}

		void RefreshTextInputSessionForActiveTarget(const char* reason)
		{
			const bool wasActive = s_textInputSessionActive;
			CancelDeferredStewieAscii();
			s_imeComposing = false;
			AdvanceTsfCandidateSession();
			ClearImePreviewState();
			HideCandidateOverlay();
			s_textInputSessionActive = true;
			if (s_window)
			{
				if (s_gameImeEnabled)
					RestoreDefaultGameImeContext(s_window, reason ? reason : "target_refresh");
				else
					SetGameImeEnabled(s_window, true);
			}

			DebugLog(
				"tnvse_multibyte_input: text input session %s reason=%s",
				wasActive ? "refreshed" : "started",
				reason ? reason : "target_refresh");
		}

		void UpdateGameImeAssociation()
		{
			if (!s_window)
				return;

			SetTextInputSessionActive(GetCurrentTextEditMenuObject() != nullptr || GetOverlayStewieInputTarget().valid);
		}

		void EndStewieTextInputSession(const char* reason)
		{
			CancelDeferredStewieAscii();
			s_imeComposing = false;
			s_nativeImeAsciiGuardUntilTick = 0;
			s_lastStewieImeCommitTick = 0;
			s_lastStewieImeEnterKeyTick = 0;
			AdvanceTsfCandidateSession();
			ClearImePreviewState();
			HideCandidateOverlay();
			SetTextInputSessionActive(false);
			UpdateGameImeAssociation();

			DebugLog(
				"tnvse_multibyte_input: Stewie text input session reset reason=%s",
				reason ? reason : "unknown");
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

		bool ShouldSuppressInputLanguageSwitchAscii(UInt8 input)
		{
			if (input != ' ')
				return false;

			return IsVirtualKeyDown(VK_LWIN)
				|| IsVirtualKeyDown(VK_RWIN)
				|| static_cast<SInt32>(s_inputLanguageSwitchGuardUntilTick - GetTickCount()) > 0;
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
			if (!g_bMultibyteInputCompositionPreview || !s_imeCandidateState.imeOpen)
				return lines;

			if (!HasOverlayInputTarget())
			{
				ClearStewieInputState();
				return lines;
			}

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
			if (!g_bMultibyteInputCompositionPreview || !IsCandidateOverlayRendererAvailable())
			{
				HideCandidateOverlay();
				return;
			}

			if (!s_imeCandidateState.imeOpen)
			{
				HideCandidateOverlay();
				return;
			}

			if (!HasOverlayInputTarget())
			{
				ClearStewieInputState();
				HideCandidateOverlay();
				return;
			}

			s_candidateOverlay.visible = true;
		}

		bool RenderOverlayTexture(
			LPDIRECT3DDEVICE9 device,
			const std::vector<CandidateOverlayLine>& lines)
		{
			if (!device || lines.empty() || !IsCandidateOverlayRendererAvailable())
				return false;

			std::vector<UInt32> pixels;
			UInt32 width = 0;
			UInt32 height = 0;
			if (!RasterizeCandidateOverlay(lines, pixels, width, height)
				|| !width
				|| !height
				|| pixels.size() != static_cast<size_t>(width) * height)
			{
				return false;
			}

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
					return false;
				}

				s_candidateOverlay.textureWidth = width;
				s_candidateOverlay.textureHeight = height;
			}

			D3DLOCKED_RECT locked = {};
			if (FAILED(s_candidateOverlay.texture->LockRect(0, &locked, nullptr, 0)))
				return false;

			for (UInt32 y = 0; y < height; ++y)
			{
				std::memcpy(
					static_cast<UInt8*>(locked.pBits) + y * locked.Pitch,
					pixels.data() + static_cast<size_t>(y) * width,
					width * sizeof(UInt32));
			}
			s_candidateOverlay.texture->UnlockRect(0);
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
			if (!g_bMultibyteInputCompositionPreview
				|| !IsCandidateOverlayRendererAvailable()
				|| !s_candidateOverlay.visible)
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
				{
					s_candidateOverlay.dirty = true;
					return;
				}

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
			constexpr DWORD kImeEnterPairMs = 250;
			if (stewieTarget.valid
				&& s_lastStewieImeEnterKeyTick
				&& s_lastImeCommitTick - s_lastStewieImeEnterKeyTick <= kImeEnterPairMs)
			{
				s_lastStewieImeCommitTick = s_lastImeCommitTick;
				s_lastStewieImeEnterKeyTick = 0;
			}
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

		std::wstring GetStewieImeEnterLiteral(HWND hwnd)
		{
			std::wstring literal = GetImeCompositionString(hwnd, GCS_COMPREADSTR);
			if (literal.empty())
				literal = GetImeCompositionString(hwnd, GCS_COMPSTR);
			if (literal.empty())
				literal = s_imeCandidateState.composition;

			if (g_uiEncoding == 1)
			{
				literal.erase(
					std::remove_if(
						literal.begin(),
						literal.end(),
						[](wchar_t value)
						{
							return !((value >= L'A' && value <= L'Z')
								|| (value >= L'a' && value <= L'z'));
						}),
					literal.end());
			}

			return literal;
		}

		bool HandleStewieImeEnter(const StewieInputTarget& target)
		{
			if (!target.valid || !s_window || !IsConfiguredImeLayout(s_window))
				return false;

			std::wstring composition = GetStewieImeEnterLiteral(s_window);

			if (!composition.empty())
			{
				CancelDeferredStewieAscii();
				s_lastStewieImeEnterKeyTick = 0;
				CancelGameImeComposition(s_window);
				s_imeComposing = false;
				ClearImePreviewState();

				const bool inserted = InsertWideTextStewie(target, composition);
				EnsureConfiguredImeOpen(s_window, "menusearch_enter_literal");
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
				DebugLog(
					"tnvse_multibyte_input_event: source=MenuSearch.Enter action=commit_ime_literal chars=%u inserted=%u",
					static_cast<UInt32>(composition.size()),
					inserted ? 1 : 0);
				return true;
			}

			if (IsImeCompositionActive() || !s_imeCandidateState.candidates.empty())
			{
				s_lastStewieImeEnterKeyTick = 0;
				return true;
			}

			constexpr DWORD kImeEnterPairMs = 250;
			if (s_lastStewieImeCommitTick
				&& GetTickCount() - s_lastStewieImeCommitTick <= kImeEnterPairMs)
			{
				s_lastStewieImeCommitTick = 0;
				DebugLog("tnvse_multibyte_input_event: source=MenuSearch.Enter action=suppress_paired_ime_enter");
				return true;
			}

			return false;
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

						return HandleStewieWndProcAscii(stewieTarget, static_cast<UInt8>(wParam));
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
				if (msg == kMessage_FlushDeferredStewieAscii)
				{
					FlushDeferredStewieAscii(static_cast<UInt32>(wParam));
					return 0;
				}

				TryInstallJipTextInputHook();
				TryInstallStewieTweaksInputHooks();
				ObserveStewieMenuSearchHotkeyMessage(msg, wParam, lParam);
				ProcessStewieMenuSearchPendingStateSync();
				TextEditMenu* inputTarget = GetOverlayTextInputMenu();
				StewieInputTarget stewieOverlayTarget = GetOverlayStewieInputTarget();
				const bool hasInputTarget = inputTarget || stewieOverlayTarget.valid;
				if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					&& wParam == VK_RETURN
					&& stewieOverlayTarget.valid
					&& IsConfiguredImeLayout(hwnd))
				{
					s_lastStewieImeEnterKeyTick = GetTickCount();
				}
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
					s_inputLanguageSwitchGuardUntilTick = GetTickCount() + 250;
					SuppressStewieInputLanguageSwitchSpace();
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
					CancelDeferredStewieAscii();
					HideSystemImeWindows(hwnd);
					s_imeComposing = true;
					s_compositionEchoChecked = false;
					s_imeCandidateState.composing = true;
					RefreshImeStatus(hwnd);
					RefreshImeComposition(hwnd);
					TryRemoveCompositionEcho();
					// Candidate data is not valid until TSF or IMN_OPENCANDIDATE /
					// IMN_CHANGECANDIDATE publishes it for this composition. Reading
					// IMM here can return the previous composition's cached list.
					ClearImeCandidates();
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
						// Candidate list updates are driven by WM_IME_NOTIFY and the TSF
						// UI element sink. Do not poll IMM during composition text updates;
						// some IMEs retain the previous list until the open/change notify.
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
					if (lParam & GCS_COMPSTR)
						CancelDeferredStewieAscii();

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
					&& IsCandidateOverlayRendererAvailable()
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
			s_tsfSessionGeneration = 1;
			s_tsfUiElementSessions.clear();
			ClearImePreviewState();
			HideCandidateOverlay();
			ReleaseCandidateOverlayTexture();
			s_suppressedImeCharCount = 0;
			s_lastImeCommitTick = 0;
			s_lastStewieImeCommitTick = 0;
			s_lastStewieImeEnterKeyTick = 0;
			s_inputLanguageSwitchGuardUntilTick = 0;
			s_lastWndProcAsciiTick = 0;
			s_lastWndProcAsciiChar = 0;
			ClearJipTextInputHookState();
			ResetStewieInputState();
		}

		void RestoreWindowProc()
		{
			RestoreTextEditInputHook();

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
}
