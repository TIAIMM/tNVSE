#pragma once
#include "nvse/PluginAPI.h"

extern HMODULE hJIP;
extern PluginHandle g_pluginHandle;
extern NVSEMessagingInterface* g_messagingInterface;
extern NVSEEventManagerInterface* g_eventInterface;
extern NVSEConsoleInterface* g_consoleInterface;

static size_t __fastcall GetJIPAddress(size_t aiAddress)
{
	return reinterpret_cast<size_t>(hJIP) + aiAddress - 0x10000000;
}

extern NVSECommandTableInterface* g_cmdTableInterface;
