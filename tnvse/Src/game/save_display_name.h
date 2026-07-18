#pragma once

#include "ITypes.h"
#include "nvse/PluginAPI.h"

namespace fonthook
{
	void HandleSaveDisplayNameMessage(NVSEMessagingInterface::Message* message);
	void InitSaveDisplayNameHook();
}
