#include "nvse/PluginAPI.h"

PluginManager	g_pluginManager;

PluginManager::LoadedPlugin* PluginManager::s_currentLoadingPlugin = NULL;
PluginHandle					PluginManager::s_currentPluginHandle = 0;

UInt32 PluginManager::GetNumPlugins(void)
{
	UInt32	numPlugins = m_plugins.size();

	// is one currently loading?
	if (s_currentLoadingPlugin) numPlugins++;

	return numPlugins;
}

UInt32 PluginManager::GetBaseOpcode(UInt32 idx)
{
	return m_plugins[idx].baseOpcode;
}

PluginHandle PluginManager::LookupHandleFromBaseOpcode(UInt32 baseOpcode)
{
	UInt32	idx = 1;

	for (LoadedPluginList::iterator iter = m_plugins.begin(); iter != m_plugins.end(); ++iter)
	{
		LoadedPlugin* plugin = &(*iter);

		if (plugin->baseOpcode == baseOpcode)
			return idx;

		idx++;
	}

	return kPluginHandle_Invalid;
}

PluginInfo* PluginManager::GetInfoByName(const char* name)
{
	for (LoadedPluginList::iterator iter = m_plugins.begin(); iter != m_plugins.end(); ++iter)
	{
		LoadedPlugin* plugin = &(*iter);

		if (plugin->info.name && !_stricmp(name, plugin->info.name))
			return &plugin->info;
	}

	return NULL;
}

PluginInfo* PluginManager::GetInfoFromHandle(PluginHandle handle)
{
	if (handle > 0 && handle <= m_plugins.size())
		return &m_plugins[handle - 1].info;

	return NULL;
}

PluginInfo* PluginManager::GetInfoFromBase(UInt32 baseOpcode)
{
	PluginHandle	handle = LookupHandleFromBaseOpcode(baseOpcode);

	if (baseOpcode && handle > 0 && handle <= m_plugins.size())
		return &m_plugins[handle - 1].info;

	return NULL;
}

PluginInfo* PluginManager::GetInfoByDLLName(const char* DLLName) {
	HMODULE module = GetModuleHandleA(DLLName);
	if (!module)
		return nullptr;

	for (LoadedPluginList::iterator iter = m_plugins.begin(); iter != m_plugins.end(); ++iter)
	{
		LoadedPlugin* plugin = &(*iter);
		if (plugin->handle == module)
			return &plugin->info;
	}

	return nullptr;
}

const char* PluginManager::GetPluginNameFromHandle(PluginHandle handle)
{
	if (handle > 0 && handle <= m_plugins.size())
		return (m_plugins[handle - 1].info.name);
	else if (handle == 0)
		return "NVSE";

	return NULL;
}