#pragma once

#include "nvse/PluginAPI.h"

namespace fonthook
{
	void InitMultibyteInputHook();
	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage);
}
