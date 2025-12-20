#include <ranges>

#include "NiTriShape.hpp"
#include "PluginAPI.h"
#include "BSFadeNode.hpp"
#include "BSShaderManager.hpp"
#include "BSTextureManager.hpp"
#include "NiDX9Renderer.hpp"
#include "NiRenderer.hpp"
#include "PlayerCharacter.hpp"
#include "ShadowSceneNode.hpp"
#include "InterfaceManager.hpp"
#include "loadconfig.h"
#include "fonthook.h"

IDebugLog gLog("tnvse.log");
PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSEEventManagerInterface* g_eventInterface;

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

		if (g_bReorderDoorPrompt) {
			fonthook::InitDoorPromptHooks();
		}

		if (g_bRemovePlural) {
			fonthook::InitPluralHooks();
		}

		fonthook::InitVertSpacingHook();
		fonthook::InitFontHook();
		fonthook::InitJIPHooks();
	}

	return true;
}
