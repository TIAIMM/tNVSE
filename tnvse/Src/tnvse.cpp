//#include <ranges>
//#include "NiTriShape.hpp"
//#include "BSFadeNode.hpp"
//#include "BSShaderManager.hpp"
//#include "BSTextureManager.hpp"
//#include "NiDX9Renderer.hpp"
//#include "NiRenderer.hpp"
//#include "PlayerCharacter.hpp"
//#include "ShadowSceneNode.hpp"
//#include "InterfaceManager.hpp"
//#include "nvse/PluginAPI.h"
#include "load_config.h"
#include "tnvse.h"
#include "font_hook.h"
#include "font_vector.h"
#include "dictionary.h"
#include "multibyte_input.h"
#include "native_tile_overlay.h"
#include "plugin_dependencies.h"
#include "save_display_name.h"
#include "tianmiao_conflict.h"

#include <cstdint>
#include <vector>

IDebugLog gLog("tnvse.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSEEventManagerInterface* g_eventInterface{};
NVSEConsoleInterface* g_consoleInterface{};
HMODULE hJIP = 0;
NVSECommandTableInterface* g_cmdTableInterface = NULL;

void MessageHandler(NVSEMessagingInterface::Message* const g_msg);

// Config

namespace
{
	constexpr UInt32 kSupportedRuntimeVersion = 0x040020D0;
	bool s_shaderLoaderListenerRegistered = false;

	bool TryRegisterShaderLoaderListener()
	{
		if (s_shaderLoaderListenerRegistered)
			return true;
		if (!g_messagingInterface
			|| g_messagingInterface->version < NVSEMessagingInterface::kVersion
			|| !g_messagingInterface->RegisterListener
			|| g_pluginHandle == kPluginHandle_Invalid)
		{
			return false;
		}

		s_shaderLoaderListenerRegistered =
			g_messagingInterface->RegisterListener(
				g_pluginHandle, "Shader Loader", MessageHandler);
		return s_shaderLoaderListenerRegistered;
	}

	void LogLoadedTnvseModuleIdentity(const void* address)
	{
		HMODULE module = nullptr;
		char modulePath[MAX_PATH] = {};
		DWORD moduleError = ERROR_SUCCESS;
		if (!address || !GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(address), &module)
			|| !GetModuleFileNameA(module, modulePath, MAX_PATH))
		{
			moduleError = GetLastError();
		}

		DWORD peTimestamp = 0;
		DWORD imageSize = 0;
		if (module)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
			if (dos->e_magic == IMAGE_DOS_SIGNATURE)
			{
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
					reinterpret_cast<const UInt8*>(module) + dos->e_lfanew);
				if (nt->Signature == IMAGE_NT_SIGNATURE)
				{
					peTimestamp = nt->FileHeader.TimeDateStamp;
					imageSize = nt->OptionalHeader.SizeOfImage;
				}
			}
		}

		UInt64 fileBytes = 0;
		UInt64 fileWriteTime = 0;
		UInt64 fileHash = 14695981039346656037ull;
		DWORD fileError = ERROR_SUCCESS;
		HANDLE file = modulePath[0]
			? CreateFileA(modulePath, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
			: INVALID_HANDLE_VALUE;
		if (file == INVALID_HANDLE_VALUE)
		{
			fileError = GetLastError();
			fileHash = 0;
		}
		else
		{
			FILETIME writeTime = {};
			if (GetFileTime(file, nullptr, nullptr, &writeTime))
			{
				fileWriteTime = static_cast<UInt64>(writeTime.dwHighDateTime)
					<< 32 | writeTime.dwLowDateTime;
			}
			std::vector<UInt8> bytes(16u * 1024u);
			for (;;)
			{
				DWORD readBytes = 0;
				if (!ReadFile(file, bytes.data(),
					static_cast<DWORD>(bytes.size()), &readBytes, nullptr))
				{
					fileError = GetLastError();
					fileHash = 0;
					break;
				}
				if (!readBytes)
					break;
				fileBytes += readBytes;
				for (DWORD index = 0; index < readBytes; ++index)
				{
					fileHash ^= bytes[index];
					fileHash *= 1099511628211ull;
				}
			}
			CloseHandle(file);
		}

		gLog.FormattedMessage(
			"tnvse_build_identity: diagnostics=dynamic-ui-published-proof-witness-v100 module=%s base=%p peTimestamp=0x%08X imageSize=%u fileBytes=%llu fileWriteTime=0x%016llX fnv1a64=0x%016llX moduleError=%u fileError=%u",
			modulePath[0] ? modulePath : "unresolved", module,
			peTimestamp, imageSize,
			static_cast<unsigned long long>(fileBytes),
			static_cast<unsigned long long>(fileWriteTime),
			static_cast<unsigned long long>(fileHash),
			moduleError, fileError);
	}

	bool s_configuredGameFontsPrepared = false;
	UInt32 s_configuredGameFontPrepareAttempts = 0;
	constexpr UInt32 kMaximumConfiguredGameFontPrepareAttempts = 120;

	const char* FontPrewarmPumpStatusName(
		fonthook::FontPrewarmPumpStatus status)
	{
		switch (status)
		{
		case fonthook::FontPrewarmPumpStatus::Idle:
			return "idle";
		case fonthook::FontPrewarmPumpStatus::Active:
			return "active";
		case fonthook::FontPrewarmPumpStatus::Completed:
			return "completed";
		case fonthook::FontPrewarmPumpStatus::Failed:
			return "failed";
		default:
			return "unknown";
		}
	}

	void PrepareConfiguredGameFonts()
	{
		if (s_configuredGameFontsPrepared)
			return;
		if (!g_bEnableFreeTypeFontRendering
			|| !fonthook::AreFreeTypeFontHooksInstalled())
		{
			s_configuredGameFontsPrepared = true;
			return;
		}

		FontManager* manager = FontManager::GetSingleton();
		if (!manager)
			return;
		const bool hasExtendedFonts = fonthook::HasJipExtendedFontManager();
		const bool hasConsoleRunner = g_consoleInterface
			&& g_consoleInterface->version >= NVSEConsoleInterface::kVersion
			&& g_consoleInterface->RunScriptLine2;
		const bool prepareTweakFont = hasExtendedFonts && hasConsoleRunner
			&& fonthook::IsFreeTypeFontConfigured(42);
		Font* tweakFont = nullptr;
		if (prepareTweakFont)
		{
			tweakFont = fonthook::ResolveGameFont(manager, 42);
			if (!tweakFont || tweakFont->iFontNum != 42)
			{
				g_consoleInterface->RunScriptLine2(
					"SetFontFile 42 \"textures\\fonts\\Monofonto_STn.fnt\"",
					nullptr, true);
				tweakFont = fonthook::ResolveGameFont(manager, 42);
			}
			// JIP aliases unset extended slots to a base Font object.  Do not treat
			// that alias as a successfully created font 42 if the command ran too
			// early or was rejected; keep retrying until the slot owns font 42.
			if (tweakFont && tweakFont->iFontNum != 42)
				tweakFont = nullptr;
		}

		Font* activated[fonthook::kAddressableGameFontCount] = {};
		size_t activatedCount = 0;
		bool tweakFontActivated = !prepareTweakFont;
		auto activate = [&](Font* font)
		{
			if (!font || !fonthook::IsFreeTypeFontConfigured(font->iFontNum))
				return;
			for (size_t index = 0; index < activatedCount; ++index)
			{
				if (activated[index] == font)
					return;
			}
			activated[activatedCount++] = font;
			const bool active = fonthook::ActivateFreeTypeFont(font);
			if (font == tweakFont)
				tweakFontActivated = active;
		};
		for (Font* font : manager->pFont)
			activate(font);
		// The retail manager ends at offset 0x24. Only read JIP's appended slots
		// after its SetFontFile command proves the extended manager is installed.
		if (hasExtendedFonts)
		{
			for (Font* font : manager->extraFonts)
				activate(font);
		}
		++s_configuredGameFontPrepareAttempts;
		s_configuredGameFontsPrepared = tweakFontActivated
			|| s_configuredGameFontPrepareAttempts
				>= kMaximumConfiguredGameFontPrepareAttempts;
	}

	void RunConfiguredGameFontPrewarmLoadingBarrier()
	{
		const ULONGLONG started = GetTickCount64();
		UInt32 steps = 0;
		fonthook::FontPrewarmPumpStatus status =
			fonthook::PumpFreeTypeFontPrewarm();
		++steps;
		if (status == fonthook::FontPrewarmPumpStatus::Idle)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: DeferredInit LoadingMenu prewarm barrier skipped status=idle steps=%u",
				steps);
			return;
		}

		gLog.FormattedMessage(
			"tnvse_freetype_font: DeferredInit LoadingMenu prewarm barrier begin status=%s",
			FontPrewarmPumpStatusName(status));
		while (status == fonthook::FontPrewarmPumpStatus::Active)
		{
			// Keep the host window responsive while DeferredInit deliberately
			// blocks gameplay until the cache transaction reaches a terminal state.
			// The prewarm service dispatches only a bounded number of messages and
			// rejects a re-entrant cache pump.
			fonthook::ServiceFreeTypeFontPrewarmHostMessages();
			Sleep(0);
			status = fonthook::PumpFreeTypeFontPrewarm();
			++steps;
			if ((steps & 0x3F) == 0)
				fonthook::FlushFreeTypeFontDebugLog();
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: DeferredInit LoadingMenu prewarm barrier end status=%s steps=%u elapsedMs=%llu",
			FontPrewarmPumpStatusName(status),
			steps,
			static_cast<unsigned long long>(GetTickCount64() - started));
	}
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void MessageHandler(NVSEMessagingInterface::Message* const g_msg)
{
	if (g_msg && g_msg->sender && std::strcmp(g_msg->sender, "Shader Loader") == 0)
	{
		if (fonthook::AreFreeTypeFontHooksInstalled())
			fonthook::HandleFreeTypeShaderLoaderMessage(g_msg->type);
		return;
	}
	if (g_msg && g_msg->sender
		&& std::strcmp(g_msg->sender, "NVSE") == 0
		&& g_msg->type == NVSEMessagingInterface::kMessage_PostLoad)
	{
		// Registering for another plugin during NVSEPlugin_Load fails whenever
		// that plugin appears later in load order. PostLoad is the first point at
		// which every sender is discoverable; RegisterListener is idempotent for
		// an already registered listener.
		TryRegisterShaderLoaderListener();
		fonthook::ReconcileSaveDisplayNameHookPostLoad();
	}
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		fonthook::dependencies::ShowExternalPluginDependencyWarnings();
		if (fonthook::AreFreeTypeFontHooksInstalled())
		{
			fonthook::FinalizeFreeTypeNativeRendererDetection();
			fonthook::InitializeFreeTypeDefaultPoolAtlas();
			PrepareConfiguredGameFonts();
			RunConfiguredGameFontPrewarmLoadingBarrier();
		}
	}
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		if (fonthook::AreFreeTypeFontHooksInstalled())
		{
			PrepareConfiguredGameFonts();
			fonthook::HandleFreeTypeNativeRendererMainLoop();
			fonthook::HandleFreeTypeDefaultPoolAtlasMainLoop();
			fonthook::PumpFreeTypeFontPerformance();
		}
	}
	if (g_msg && (g_msg->type == NVSEMessagingInterface::kMessage_ExitGame
		|| g_msg->type == NVSEMessagingInterface::kMessage_ExitGame_Console))
	{
		// The ten-second periodic report can precede the menu under test and the
		// process can exit before the next interval. Preserve the final rendering
		// counters before resource teardown so diagnostics describe the visible run.
		fonthook::ReportFreeTypeFontPerformanceNow();
		fonthook::ShutdownFreeTypeFontPrewarm();
		fonthook::FlushFreeTypePersistentFontCache();
		fonthook::ShutdownFreeTypeDefaultPoolAtlas();
		fonthook::ShutdownNativeTileOverlayHost();
		fonthook::FlushFreeTypeFontDebugLogFully();
	}
	if (g_msg && (g_msg->type == NVSEMessagingInterface::kMessage_DeferredInit
		|| g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop))
	{
		fonthook::FlushFreeTypeFontDebugLog();
	}
	fonthook::HandleSaveDisplayNameMessage(g_msg);
	fonthook::HandleMultibyteInputMessage(g_msg);
}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	if (!nvse || !info)
		return false;
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "tNVSE";
	info->version = 75;

	// Every game/renderer entry used by tNVSE is a fixed address in the retail
	// 1.4.0.525 image. In particular, the NoGore 1.4.0.525 executable has a
	// distinct packed version and must not reach any hook installer.
	return nvse->isEditor
		|| nvse->runtimeVersion == kSupportedRuntimeVersion;
}

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	if (!nvse)
		return false;
	if (nvse->isEditor)
	{
		return true;
	}
	if (nvse->runtimeVersion != kSupportedRuntimeVersion
		|| !nvse->QueryInterface || !nvse->GetPluginHandle)
	{
		gLog.FormattedMessage(
			"tnvse: unsupported runtime or incomplete NVSE root interface runtime=%08X expected=%08X query=%p handle=%p",
			nvse->runtimeVersion, kSupportedRuntimeVersion,
			nvse->QueryInterface, nvse->GetPluginHandle);
		return false;
	}
	if (fonthook::compatibility::BlockTianmiaoFontPatchIfPresent(
		"plugin-load-entry"))
		return false;

	LoadConfig();
	LogLoadedTnvseModuleIdentity(
		reinterpret_cast<const void*>(&NVSEPlugin_Load));
	fonthook::LoadFreeTypeFontConfig();
	fonthook::LoadDictionaryConfig();

	g_nvseInterface = const_cast<NVSEInterface*>(nvse);
	g_pluginHandle = nvse->GetPluginHandle();
	g_messagingInterface = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
	g_consoleInterface = static_cast<NVSEConsoleInterface*>(nvse->QueryInterface(kInterface_Console));
	g_eventInterface = static_cast<NVSEEventManagerInterface*>(nvse->QueryInterface(kInterface_EventManager));
	g_cmdTableInterface = static_cast<NVSECommandTableInterface*>(
		nvse->QueryInterface(kInterface_CommandTable));
	if (g_pluginHandle == kPluginHandle_Invalid
		|| !g_messagingInterface
		|| g_messagingInterface->version < NVSEMessagingInterface::kVersion
		|| !g_messagingInterface->RegisterListener)
	{
		gLog.FormattedMessage(
			"tnvse: required xNVSE messaging interface unavailable handle=%08X interface=%p version=%u register=%p",
			g_pluginHandle, g_messagingInterface,
			g_messagingInterface ? g_messagingInterface->version : 0,
			g_messagingInterface
				? g_messagingInterface->RegisterListener : nullptr);
		return false;
	}
	if (g_cmdTableInterface
		&& g_cmdTableInterface->version < NVSECommandTableInterface::kVersion)
	{
		g_cmdTableInterface = nullptr;
	}

	hJIP = GetModuleHandle("jip_nvse.dll");

	// No executable or module hook may be installed before the final condition
	// that can reject NVSEPlugin_Load. xNVSE unloads a plugin that returns false;
	// leaving any branch into that DLL behind would create a dangling target.
	if (fonthook::compatibility::BlockTianmiaoFontPatchIfPresent(
		"before-font-hooks"))
	{
		return false;
	}
	if (!g_messagingInterface->RegisterListener(
			g_pluginHandle, "NVSE", MessageHandler))
	{
		gLog.FormattedMessage(
			"tnvse: failed to register the required NVSE message listener");
		return false;
	}
	TryRegisterShaderLoaderListener();

	if (g_bEnableFreeTypeFontRendering)
		fonthook::InstallNativePrewarmOverlayLoadingThreadHook();

	if (g_bChangeJIPBigGunDesc)
	{
		if (hJIP)
		{
			const PluginInfo* pInfo = g_cmdTableInterface
				&& g_cmdTableInterface->GetPluginInfoByName
				? g_cmdTableInterface->GetPluginInfoByName(
					fonthook::dependencies::kJipPluginName) : nullptr;
			if (fonthook::dependencies::IsPluginInfoValid(pInfo)
				&& pInfo->version == fonthook::dependencies::kJipBigGunsVersion)
			{
				fonthook::InitBigGunsDescHooks();
			}
		}
	}

	if (g_uiReorderDoorPrompt == 1)
	{
		fonthook::InitDoorPromptHooksCHS();
	}
	else if (g_uiReorderDoorPrompt == 2)
	{
		fonthook::InitDoorPromptHooksKOR();
	}

	if (g_bRemovePlural)
	{
		fonthook::InitPluralHooks();
	}



	fonthook::InitFontHooks();

	fonthook::InitMultibyteInputHook();

	fonthook::InitSaveDisplayNameHook();

	return true;
}
