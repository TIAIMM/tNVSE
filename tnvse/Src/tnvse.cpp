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
#include "dictionary.h"
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
	fonthook::HandleSaveDisplayNameMessage(g_msg);

	if (g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
	}
}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "tNVSE";
	info->version = 1;

	nvse->SetOpcodeBase(fonthook::kTNVSEOpcodeBase);

	return true;
}

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	if (nvse->isEditor)
	{
		return true;
	}

	LoadConfig();
	fonthook::LoadDictionaryConfig();

	g_nvseInterface = const_cast<NVSEInterface*>(nvse);
	g_pluginHandle = nvse->GetPluginHandle();
	g_messagingInterface = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
	if (g_messagingInterface)
		g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);

	auto* serializationInterface = static_cast<NVSESerializationInterface*>(nvse->QueryInterface(kInterface_Serialization));
	fonthook::InitSaveDisplayName(serializationInterface);

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

	if (g_bSaveDisplayNameMap)
	{
		fonthook::InitSaveDisplayNameHook();
	}

	return true;
}
