#pragma once

#include "ITypes.h"
#include "nvse/PluginAPI.h"

namespace fonthook
{
	static constexpr UInt32 kTNVSEOpcodeBase = 0x4297;

	void InitSaveDisplayName(NVSESerializationInterface* serialization);
	void HandleSaveDisplayNameMessage(NVSEMessagingInterface::Message* message);
	void InitSaveDisplayNameHook();
}
