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
#include "loadconfig.h"
#include "tnvse.h"
#include "fonthook.h"

IDebugLog gLog("tnvse.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSEEventManagerInterface* g_eventInterface;
HMODULE hJIP = 0;
NVSECommandTableInterface* g_cmdTableInterface = NULL;

// Config

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void MessageHandler(NVSEMessagingInterface::Message* const g_msg) {
	if (g_msg->type == NVSEMessagingInterface::kMessage_MainGameLoop) {
	}
}

bool NVSEPlugin_Query(const NVSEInterface*, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "tNVSE";
	info->version = 1;

	return true;
}

bool NVSEPlugin_Load(const NVSEInterface* nvse) {
	if (!nvse->isEditor) {
		LoadConfig();

		hJIP = GetModuleHandle("jip_nvse.dll");
		g_cmdTableInterface = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

		if (g_bChangeJIPBigGunDesc) {
			if (hJIP) {
				const PluginInfo* pInfo = g_cmdTableInterface->GetPluginInfoByName("JIP LN NVSE");
				if (pInfo->version == 5730) {
					fonthook::InitBigGunsDescHooks();
				}
			}
		}

		if (g_uiReorderDoorPrompt == 1) {
			fonthook::InitDoorPromptHooksCHS();
		}
		else if (g_uiReorderDoorPrompt == 2) {
			fonthook::InitDoorPromptHooksKOR();
		}

		if (g_bRemovePlural) {
			fonthook::InitPluralHooks();
		}

		fonthook::InitVertSpacingHook();

		fonthook::InitFontHook();
	}

	return true;
}
