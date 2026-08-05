#include "multibyte_input_ime_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		void AdvanceTsfCandidateSession()
		{
			if (++State().tsfSessionGeneration == 0)
			{
				State().tsfSessionGeneration = 1;
				State().tsfUiElementSessions.clear();
			}
		}

		bool RegisterTsfUiElement(DWORD id)
		{
			ImeState& state = State();
			const auto existing = std::find_if(
				state.tsfUiElementSessions.begin(),
				state.tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing != state.tsfUiElementSessions.end())
			{
				// The same TSF id can be reused before the old EndUIElement is
				// delivered. Keep every outstanding Begin reference so an old End
				// cannot erase the newly published candidate list.
				if (existing->beginCount != static_cast<UInt32>(-1))
					++existing->beginCount;
				existing->generation = state.tsfSessionGeneration;
				return true;
			}

			constexpr size_t kMaxRememberedTsfUiElements = 64;
			if (state.tsfUiElementSessions.size() >= kMaxRememberedTsfUiElements)
				state.tsfUiElementSessions.erase(state.tsfUiElementSessions.begin());
			state.tsfUiElementSessions.push_back(
				{ id, state.tsfSessionGeneration, 1 });
			return true;
		}

		bool IsCurrentTsfUiElement(DWORD id)
		{
			return std::any_of(
				State().tsfUiElementSessions.begin(),
				State().tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id && value.generation == State().tsfSessionGeneration;
				});
		}

		bool ReleaseTsfUiElement(DWORD id)
		{
			ImeState& state = State();
			const auto existing = std::find_if(
				state.tsfUiElementSessions.begin(),
				state.tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing == state.tsfUiElementSessions.end())
				return false;

			const bool wasCurrent =
				existing->generation == state.tsfSessionGeneration;
			if (existing->beginCount > 1)
			{
				--existing->beginCount;
				return false;
			}
			state.tsfUiElementSessions.erase(existing);
			return wasCurrent;
		}

		bool HasCurrentTsfUiElement()
		{
			return std::any_of(
				State().tsfUiElementSessions.begin(),
				State().tsfUiElementSessions.end(),
				[](const TsfUiElementSession& value)
				{
					return value.generation == State().tsfSessionGeneration;
				});
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

		struct TsfPendingUpdate
		{
			bool profileChanged = false;
			bool compositionChanged = false;
			UInt32 compositionGeneration = 0;
			std::wstring composition;
		};

		class TsfCandidateSink final
			: public ITfUIElementSink
			, public ITfInputProcessorProfileActivationSink
			, public ITfThreadMgrEventSink
			, public ITfTextEditSink
		{
		public:
			TsfCandidateSink()
			{
				InitializeSRWLock(&m_pendingLock);
			}

			~TsfCandidateSink()
			{
				Shutdown();
			}

			STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
			{
				if (!ppvObj)
					return E_INVALIDARG;

				*ppvObj = nullptr;
				if (IsEqualIID(riid, IID_IUnknown)
					|| IsEqualIID(riid, IID_ITfUIElementSink))
				{
					*ppvObj = static_cast<ITfUIElementSink*>(this);
				}
				else if (IsEqualIID(
					riid,
					IID_ITfInputProcessorProfileActivationSink))
				{
					*ppvObj =
						static_cast<ITfInputProcessorProfileActivationSink*>(this);
				}
				else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
				{
					*ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
				}
				else if (IsEqualIID(riid, IID_ITfTextEditSink))
				{
					*ppvObj = static_cast<ITfTextEditSink*>(this);
				}
				else
					return E_NOINTERFACE;

				AddRef();
				return S_OK;
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
				// TSF invokes this as a COM callback. Only capture candidate data
				// here; game-menu discovery and overlay updates belong to the main
				// loop so a text-service callback can never re-enter Gamebryo UI.
				const ULONG_PTR publishedWindow =
					State().tsfInputWindow.load(std::memory_order_acquire);
				HWND overlayWindow =
					reinterpret_cast<HWND>(publishedWindow);
				const bool hasOverlayTarget =
					overlayWindow
					&& State().textInputSessionActive.load(
						std::memory_order_acquire);
				const bool isCandidateElement =
					IsCandidateElement(dwUIElementId);
				const bool captureCandidates =
					g_bMultibyteInputCompositionPreview
					&& g_bMultibyteInputUseTSFCandidates;
				const bool compositionOrHandoff = s_imeComposing
					|| IsImeResultHandoffActive(GetTickCount());
				const bool candidateRegistered =
					captureCandidates
					&& hasOverlayTarget
					&& isCandidateElement
					&& RegisterTsfUiElement(dwUIElementId);
				const bool acceptsCandidates = compositionOrHandoff
					&& hasOverlayTarget
					&& candidateRegistered;
				// A text service is allowed to expose candidate/reading UI as a
				// generic ITfUIElement without ITfCandidateListUIElement. MCM
				// Extender's script-backed search triggers that path with some
				// modern IMEs. Suppress every TSF-owned UI element; the interface
				// test below is only for deciding whether candidate data can be
				// mirrored into the native Tile overlay.
				if (pbShow && *pbShow != FALSE)
					*pbShow = FALSE;

				if (!captureCandidates)
					return S_OK;
				if (!acceptsCandidates)
					return S_OK;

				ReadCandidateElement(dwUIElementId);
				State().overlayRefreshPending = true;
				return S_OK;
			}

			STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override
			{
				if (!g_bMultibyteInputCompositionPreview
					|| !g_bMultibyteInputUseTSFCandidates)
				{
					return S_OK;
				}
				if ((!s_imeComposing
						&& !IsImeResultHandoffActive(GetTickCount()))
					|| !State().textInputSessionActive
					|| !IsCurrentTsfUiElement(dwUIElementId))
					return S_OK;

				ReadCandidateElement(dwUIElementId);
				State().overlayRefreshPending = true;
				return S_OK;
			}

			STDMETHODIMP EndUIElement(DWORD dwUIElementId) override
			{
				if (!ReleaseTsfUiElement(dwUIElementId))
					return S_OK;
				if (HasCurrentTsfUiElement())
					return S_OK;

				State().tsfCandidateActive = false;
				if (State().candidate.candidatesFromTsf)
					ClearImeCandidates();
				State().overlayRefreshPending = true;
				return S_OK;
			}

			STDMETHODIMP OnActivated(
				DWORD,
				LANGID,
				REFCLSID,
				REFGUID,
				REFGUID,
				HKL,
				DWORD dwFlags) override
			{
				if (dwFlags & TF_IPSINK_FLAG_ACTIVE)
					PublishProfileChanged();
				return S_OK;
			}

			STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override
			{
				return S_OK;
			}

			STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override
			{
				return S_OK;
			}

			STDMETHODIMP OnSetFocus(
				ITfDocumentMgr* document,
				ITfDocumentMgr*) override
			{
				AttachTextEditSink(document);
				return S_OK;
			}

			STDMETHODIMP OnPushContext(ITfContext*) override
			{
				return S_OK;
			}

			STDMETHODIMP OnPopContext(ITfContext*) override
			{
				return S_OK;
			}

			STDMETHODIMP OnEndEdit(
				ITfContext* context,
				TfEditCookie readCookie,
				ITfEditRecord*) override
			{
				PublishComposition(
					ReadCompositionText(context, readCookie),
					State().textInputSessionGeneration);
				return S_OK;
			}

			void PumpPendingUpdates()
			{
				if (!m_pendingDirty.exchange(
						false,
						std::memory_order_acquire))
				{
					return;
				}

				TsfPendingUpdate pending;
				AcquireSRWLockExclusive(&m_pendingLock);
				pending = std::move(m_pending);
				m_pending = {};
				ReleaseSRWLockExclusive(&m_pendingLock);

				if (pending.profileChanged && s_window)
				{
					m_currentInputMethodName =
						QueryCurrentInputMethodName();
					RefreshImeStatus(s_window);
					State().overlayRefreshPending = true;
					DebugLog(
						"tnvse_multibyte_input: TSF input profile activation observed");
				}

				if (!pending.compositionChanged
					|| !State().textInputSessionActive
					|| pending.compositionGeneration
						!= State().textInputSessionGeneration)
				{
					return;
				}

				ImeState& state = State();
				if (!pending.composition.empty())
				{
					if (state.candidate.composition.empty()
						|| state.tsfCompositionFallbackActive)
					{
						state.candidate.composition =
							std::move(pending.composition);
						state.candidate.composing = true;
						state.tsfCompositionFallbackActive = true;
						s_imeComposing = true;
						MarkImeResultHandoffComposition();
						state.overlayRefreshPending = true;
					}
					return;
				}

				if (!state.tsfCompositionFallbackActive)
					return;

				const std::wstring immComposition = s_window
					? GetImeCompositionString(s_window, GCS_COMPSTR)
					: std::wstring();
				state.tsfCompositionFallbackActive = false;
				if (!immComposition.empty())
				{
					state.candidate.composition = immComposition;
					state.candidate.composing = true;
				}
				else
				{
					state.candidate.composition.clear();
					state.candidate.composing = false;
					s_imeComposing = false;
				}
				state.overlayRefreshPending = true;
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
				if (FAILED(hr))
				{
					SafeRelease(source);
					return false;
				}

				if (FAILED(source->AdviseSink(
					__uuidof(ITfInputProcessorProfileActivationSink),
					static_cast<ITfInputProcessorProfileActivationSink*>(this),
					&m_profileActivationSinkCookie)))
				{
					m_profileActivationSinkCookie = TF_INVALID_COOKIE;
				}
				if (FAILED(source->AdviseSink(
					__uuidof(ITfThreadMgrEventSink),
					static_cast<ITfThreadMgrEventSink*>(this),
					&m_threadMgrEventSinkCookie)))
				{
					m_threadMgrEventSinkCookie = TF_INVALID_COOKIE;
				}
				SafeRelease(source);
				if (m_threadMgrEventSinkCookie != TF_INVALID_COOKIE)
				{
					ITfDocumentMgr* focusedDocument = nullptr;
					if (SUCCEEDED(m_threadMgrEx->GetFocus(
							&focusedDocument))
						&& focusedDocument)
					{
						AttachTextEditSink(focusedDocument);
						SafeRelease(focusedDocument);
					}
				}

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

				m_currentInputMethodName = QueryCurrentInputMethodName();
				m_initialized = true;
				return true;
			}

			void Shutdown()
			{
				DetachTextEditSink();
				if (m_threadMgrEx)
				{
					ITfSource* source = nullptr;
					if (SUCCEEDED(m_threadMgrEx->QueryInterface(
							__uuidof(ITfSource),
							reinterpret_cast<void**>(&source)))
						&& source)
					{
						if (m_uiElementSinkCookie != TF_INVALID_COOKIE)
							source->UnadviseSink(m_uiElementSinkCookie);
						if (m_profileActivationSinkCookie
							!= TF_INVALID_COOKIE)
						{
							source->UnadviseSink(
								m_profileActivationSinkCookie);
						}
						if (m_threadMgrEventSinkCookie
							!= TF_INVALID_COOKIE)
						{
							source->UnadviseSink(
								m_threadMgrEventSinkCookie);
						}
						SafeRelease(source);
					}

					if (m_activated)
						m_threadMgrEx->Deactivate();
				}

				m_uiElementSinkCookie = TF_INVALID_COOKIE;
				m_profileActivationSinkCookie = TF_INVALID_COOKIE;
				m_threadMgrEventSinkCookie = TF_INVALID_COOKIE;
				m_activated = false;
				m_initialized = false;
				m_pendingDirty.store(false, std::memory_order_release);
				m_currentInputMethodName.clear();
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
				return m_currentInputMethodName;
			}

		private:
			std::wstring QueryCurrentInputMethodName() const
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

			void PublishProfileChanged()
			{
				AcquireSRWLockExclusive(&m_pendingLock);
				m_pending.profileChanged = true;
				ReleaseSRWLockExclusive(&m_pendingLock);
				m_pendingDirty.store(true, std::memory_order_release);
			}

			void PublishComposition(
				std::wstring composition,
				UInt32 generation)
			{
				AcquireSRWLockExclusive(&m_pendingLock);
				m_pending.compositionChanged = true;
				m_pending.compositionGeneration = generation;
				m_pending.composition = std::move(composition);
				ReleaseSRWLockExclusive(&m_pendingLock);
				m_pendingDirty.store(true, std::memory_order_release);
			}

			void DetachTextEditSink()
			{
				if (m_textEditContext
					&& m_textEditSinkCookie != TF_INVALID_COOKIE)
				{
					ITfSource* source = nullptr;
					if (SUCCEEDED(m_textEditContext->QueryInterface(
							__uuidof(ITfSource),
							reinterpret_cast<void**>(&source)))
						&& source)
					{
						source->UnadviseSink(m_textEditSinkCookie);
						SafeRelease(source);
					}
				}

				m_textEditSinkCookie = TF_INVALID_COOKIE;
				SafeRelease(m_textEditContext);
			}

			void AttachTextEditSink(ITfDocumentMgr* document)
			{
				DetachTextEditSink();
				if (!document)
					return;

				ITfContext* context = nullptr;
				if (FAILED(document->GetBase(&context)) || !context)
					return;

				ITfSource* source = nullptr;
				if (FAILED(context->QueryInterface(
						__uuidof(ITfSource),
						reinterpret_cast<void**>(&source)))
					|| !source)
				{
					SafeRelease(context);
					return;
				}

				DWORD cookie = TF_INVALID_COOKIE;
				const HRESULT hr = source->AdviseSink(
					__uuidof(ITfTextEditSink),
					static_cast<ITfTextEditSink*>(this),
					&cookie);
				SafeRelease(source);
				if (FAILED(hr))
				{
					SafeRelease(context);
					return;
				}

				m_textEditContext = context;
				m_textEditSinkCookie = cookie;
			}

			std::wstring ReadCompositionText(
				ITfContext* context,
				TfEditCookie readCookie)
			{
				if (!context)
					return {};

				ITfContextComposition* contextComposition = nullptr;
				if (FAILED(context->QueryInterface(
						__uuidof(ITfContextComposition),
						reinterpret_cast<void**>(&contextComposition)))
					|| !contextComposition)
				{
					return {};
				}

				IEnumITfCompositionView* compositions = nullptr;
				if (FAILED(contextComposition->EnumCompositions(
						&compositions))
					|| !compositions)
				{
					SafeRelease(contextComposition);
					return {};
				}

				constexpr size_t kMaxTsfCompositionCharacters = 1024;
				std::wstring result;
				ITfCompositionView* compositionView = nullptr;
				ULONG fetchedComposition = 0;
				while (result.size() < kMaxTsfCompositionCharacters
					&& compositions->Next(
						1,
						&compositionView,
						&fetchedComposition) == S_OK
					&& fetchedComposition == 1)
				{
					ITfRange* range = nullptr;
					if (SUCCEEDED(compositionView->GetRange(&range))
						&& range)
					{
						while (result.size()
							< kMaxTsfCompositionCharacters)
						{
							wchar_t buffer[256] = {};
							ULONG fetchedText = 0;
							const ULONG capacity = static_cast<ULONG>(
								std::min<size_t>(
									255,
									kMaxTsfCompositionCharacters
										- result.size()));
							if (!capacity
								|| FAILED(range->GetText(
									readCookie,
									TF_TF_MOVESTART,
									buffer,
									capacity,
									&fetchedText))
								|| !fetchedText)
							{
								break;
							}

							result.append(buffer, fetchedText);
							if (fetchedText < capacity)
								break;
						}
						SafeRelease(range);
					}
					SafeRelease(compositionView);
					fetchedComposition = 0;
				}

				SafeRelease(compositions);
				SafeRelease(contextComposition);
				return result;
			}

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

			bool IsCandidateElement(DWORD id) const
			{
				ITfUIElement* element = GetUIElement(id);
				if (!element)
					return false;

				ITfCandidateListUIElement* candidate = nullptr;
				const bool isCandidate =
					SUCCEEDED(element->QueryInterface(
						__uuidof(ITfCandidateListUIElement),
						reinterpret_cast<void**>(&candidate)))
					&& candidate;
				SafeRelease(candidate);
				SafeRelease(element);
				return isCandidate;
			}

			void ReadCandidateElement(DWORD id)
			{
				if (m_readingCandidateElement)
					return;
				struct ReadGuard
				{
					explicit ReadGuard(bool& active) : m_active(active)
					{
						m_active = true;
					}
					~ReadGuard()
					{
						m_active = false;
					}
					bool& m_active;
				} guard(m_readingCandidateElement);

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

				State().tsfCandidateActive = true;
				State().candidate.candidatesFromTsf = true;
				State().candidate.selection = selection;
				State().candidate.pageStart = pageStart;
				State().candidate.pageSize = pageSize;
				State().candidate.candidates = std::move(candidates);
				MarkImeResultHandoffCandidates();
			}

			LONG m_refCount = 1;
			bool m_initialized = false;
			bool m_activated = false;
			bool m_coInitialized = false;
			TfClientId m_clientId = TF_CLIENTID_NULL;
			DWORD m_uiElementSinkCookie = TF_INVALID_COOKIE;
			DWORD m_profileActivationSinkCookie = TF_INVALID_COOKIE;
			DWORD m_threadMgrEventSinkCookie = TF_INVALID_COOKIE;
			DWORD m_textEditSinkCookie = TF_INVALID_COOKIE;
			ITfThreadMgrEx* m_threadMgrEx = nullptr;
			ITfInputProcessorProfileMgr* m_profileMgr = nullptr;
			ITfContext* m_textEditContext = nullptr;
			bool m_readingCandidateElement = false;
			SRWLOCK m_pendingLock;
			TsfPendingUpdate m_pending;
			std::atomic_bool m_pendingDirty = false;
			std::wstring m_currentInputMethodName;
		};

		bool InitializeTsfCandidateSupport()
		{
			if (State().tsfCandidateSink)
				return true;

			State().tsfCandidateSink.reset(new TsfCandidateSink());
			if (!State().tsfCandidateSink->Initialize())
			{
				State().tsfCandidateSink.reset();
				return false;
			}

			return true;
		}


		void TsfCandidateSinkDeleter::operator()(TsfCandidateSink* sink) const
		{
			delete sink;
		}

		std::wstring GetCurrentTsfInputMethodName()
		{
			ImeState& state = State();
			return state.tsfCandidateSink
				? state.tsfCandidateSink->GetCurrentInputMethodName()
				: std::wstring();
		}

		void PumpTsfInputUpdates()
		{
			ImeState& state = State();
			if (state.tsfCandidateSink)
				state.tsfCandidateSink->PumpPendingUpdates();
		}

		void ShutdownTsfCandidateSupport()
		{
			ImeState& state = State();
			if (!state.tsfCandidateSink)
				return;

			state.tsfCandidateSink->Shutdown();
			state.tsfCandidateSink.reset();
		}
	}
}
