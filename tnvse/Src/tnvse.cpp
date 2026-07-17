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
#include "plugin_dependencies.h"
#include "save_display_name.h"

IDebugLog gLog("tnvse.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSEEventManagerInterface* g_eventInterface;
NVSEConsoleInterface* g_consoleInterface{};
HMODULE hJIP = 0;
NVSECommandTableInterface* g_cmdTableInterface = NULL;

// Config

namespace
{
	bool s_configuredGameFontsPrepared = false;
	UInt32 s_configuredGameFontPrepareAttempts = 0;
	constexpr UInt32 kMaximumConfiguredGameFontPrepareAttempts = 120;

	bool HasJipExtendedFontManager()
	{
		if (!hJIP)
			hJIP = GetModuleHandleA("jip_nvse.dll");
		return hJIP && g_cmdTableInterface && g_cmdTableInterface->GetByName
			&& g_cmdTableInterface->GetByName("SetFontFile");
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
		const bool hasExtendedFonts = HasJipExtendedFontManager();
		const bool hasConsoleRunner = g_consoleInterface
			&& g_consoleInterface->version >= NVSEConsoleInterface::kVersion
			&& g_consoleInterface->RunScriptLine2;
		const bool prepareTweakFont = hasExtendedFonts && hasConsoleRunner
			&& fonthook::IsFreeTypeFontConfigured(42);
		Font* tweakFont = nullptr;
		if (prepareTweakFont)
		{
			constexpr UInt32 kTweakFontIndex = 42 - 10;
			tweakFont = manager->extraFonts[kTweakFontIndex];
			if (!tweakFont || tweakFont->iFontNum != 42)
			{
				g_consoleInterface->RunScriptLine2(
					"SetFontFile 42 \"textures\\fonts\\Monofonto_STn.fnt\"",
					nullptr, true);
				tweakFont = manager->extraFonts[kTweakFontIndex];
			}
			// JIP aliases unset extended slots to a base Font object.  Do not treat
			// that alias as a successfully created font 42 if the command ran too
			// early or was rejected; keep retrying until the slot owns font 42.
			if (tweakFont && tweakFont->iFontNum != 42)
				tweakFont = nullptr;
		}

		Font* activated[88] = {};
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
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		fonthook::dependencies::ShowExternalPluginDependencyWarnings();
		if (fonthook::AreFreeTypeFontHooksInstalled())
		{
			fonthook::FinalizeFreeTypeUioDetection();
			fonthook::FinalizeFreeTypeA8Detection();
			fonthook::InitializeFreeTypeDefaultPoolAtlas();
			PrepareConfiguredGameFonts();
		}
	}
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		if (fonthook::AreFreeTypeFontHooksInstalled())
		{
			PrepareConfiguredGameFonts();
			fonthook::HandleFreeTypeA8MainLoop();
			fonthook::HandleFreeTypeDefaultPoolAtlasMainLoop();
			fonthook::PumpFreeTypeFontPrewarm();
			fonthook::PumpFreeTypeFontPerformance();
		}
	}
	if (g_msg && (g_msg->type == NVSEMessagingInterface::kMessage_ExitGame
		|| g_msg->type == NVSEMessagingInterface::kMessage_ExitGame_Console))
	{
		fonthook::FlushFreeTypePersistentFontCache();
		fonthook::ShutdownFreeTypeDefaultPoolAtlas();
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
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "tNVSE";
	info->version = 1;

	return true;
}

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	if (nvse->isEditor)
	{
		return true;
	}

	LoadConfig();
	fonthook::LoadFreeTypeFontConfig();
	fonthook::LoadDictionaryConfig();

	g_nvseInterface = const_cast<NVSEInterface*>(nvse);
	g_pluginHandle = nvse->GetPluginHandle();
	g_messagingInterface = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
	g_consoleInterface = static_cast<NVSEConsoleInterface*>(nvse->QueryInterface(kInterface_Console));
	if (g_messagingInterface)
	{
		g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
		g_messagingInterface->RegisterListener(g_pluginHandle, "Shader Loader", MessageHandler);
	}

	hJIP = GetModuleHandle("jip_nvse.dll");
	g_cmdTableInterface = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

	if (g_bChangeJIPBigGunDesc)
	{
		if (hJIP)
		{
			const PluginInfo* pInfo = g_cmdTableInterface
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

	fonthook::InitVertSpacingHook();


	fonthook::InitFontHooks();

	fonthook::InitMultibyteInputHook();

	if (g_bSaveDisplayNameMap)
	{
		fonthook::InitSaveDisplayNameHook();
	}

	return true;
}
