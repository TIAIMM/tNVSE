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
#include "save_display_name.h"

IDebugLog gLog("tnvse.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSEEventManagerInterface* g_eventInterface;
HMODULE hJIP = 0;
NVSECommandTableInterface* g_cmdTableInterface = NULL;

// Config

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void MessageHandler(NVSEMessagingInterface::Message* const g_msg)
{
	if (g_msg && g_msg->sender && std::strcmp(g_msg->sender, "Shader Loader") == 0)
	{
		fonthook::HandleFreeTypeShaderLoaderMessage(g_msg->type);
		return;
	}
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		fonthook::FinalizeFreeTypeUioDetection();
		fonthook::FinalizeFreeTypeA8Detection();
	}
	if (g_msg && g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		fonthook::HandleFreeTypeA8MainLoop();
		fonthook::PumpFreeTypeFontPrewarm();
		fonthook::PumpFreeTypeFontPerformance();
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
			const PluginInfo* pInfo = g_cmdTableInterface->GetPluginInfoByName("JIP LN NVSE");
			if (pInfo->version == 5730)
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

	if (g_bEnableMultibyteFontHook)
	{
		fonthook::InitFontHook();
	}
	else
	{
		gLog.FormattedMessage("Multibyte font hooks disabled by tnvse.ini");
	}

	fonthook::InitMultibyteInputHook();

	if (g_bSaveDisplayNameMap)
	{
		fonthook::InitSaveDisplayNameHook();
	}

	return true;
}
