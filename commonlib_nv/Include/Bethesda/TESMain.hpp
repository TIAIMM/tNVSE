#pragma once

#include "BSInputManager.hpp"
#include "BSPackedTaskQueue.hpp"
#include "BSShaderAccumulator.hpp"

class SceneGraph;
class BSAudioManager;

class TESMain {
public:
	bool					bOneMore;
	bool					bQuitGame;
	bool					bExitToMainMenu;
	bool					bGameActive;
	bool					unk04;
	bool					unk05;
	bool					bIsFlyCam;
	bool					bFreezeTime;
	HWND					hWnd;
	HINSTANCE				hInstance;
	UInt32					uiMainThreadID;
	HANDLE					hMainThread;
	UInt32* PackedTaskHeap;
	UInt32					unk1C;
	BSInputManager* pInput;
	BSAudioManager* pSound;
	BSPackedTaskQueue		kTaskQueue;
	UInt32					SecondaryPackedTaskHeap;
	UInt32					unk54;
	UInt32					unk58;
	UInt32					unk5C;
	BSPackedTaskQueue		kSecondaryTaskQueue;
	BSShaderAccumulatorPtr	spWorldAccum;
	BSShaderAccumulatorPtr	sp1stPersonAccum;
	BSShaderAccumulatorPtr	spAimDOFAccumulator;
	BSShaderAccumulatorPtr	spScreenSplatterAccum;
	BSShaderAccumulatorPtr	sp3DMenuAccumulator;

	static __forceinline TESMain* GetSingleton() { return *(TESMain**)0x11DEA0C; };

	SceneGraph* GetWorldRoot() {
		return ThisStdCall<SceneGraph*>(0x45C670, this);
	}

	static bool IsInMenuMode() {
		return *reinterpret_cast<bool*>(0x11DEA2B);
	}
};
