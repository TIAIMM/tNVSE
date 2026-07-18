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
			const auto existing = std::find_if(
				State().tsfUiElementSessions.begin(),
				State().tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing != State().tsfUiElementSessions.end())
				return existing->generation == State().tsfSessionGeneration;

			constexpr size_t kMaxRememberedTsfUiElements = 64;
			if (State().tsfUiElementSessions.size() >= kMaxRememberedTsfUiElements)
				State().tsfUiElementSessions.erase(State().tsfUiElementSessions.begin());
			State().tsfUiElementSessions.push_back({ id, State().tsfSessionGeneration });
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
			const auto existing = std::find_if(
				State().tsfUiElementSessions.begin(),
				State().tsfUiElementSessions.end(),
				[id](const TsfUiElementSession& value)
				{
					return value.id == id;
				});
			if (existing == State().tsfUiElementSessions.end())
				return false;

			const bool wasCurrent = existing->generation == State().tsfSessionGeneration;
			State().tsfUiElementSessions.erase(existing);
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

				State().tsfCandidateActive = false;
				if (State().candidate.candidatesFromTsf)
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

				State().tsfCandidateActive = true;
				State().candidate.candidatesFromTsf = true;
				State().candidate.selection = selection;
				State().candidate.pageStart = pageStart;
				State().candidate.pageSize = pageSize;
				State().candidate.candidates = std::move(candidates);
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
