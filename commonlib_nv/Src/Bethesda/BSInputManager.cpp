#include "BSInputManager.hpp"

LPCDIDATAFORMAT BSInputManager::GetMouseDataFormat() {
    return LPCDIDATAFORMAT(0x1100AAC);
}

LPCDIDATAFORMAT BSInputManager::GetKeyboardDataFormat() {
    return LPCDIDATAFORMAT(0x1100CB4);
}

// 0x7FDF30
BSInputManager* BSInputManager::GetSingleton() {
    return *(BSInputManager**)0x11F35CC;
}

LPDIRECTINPUT8A BSInputManager::GetDirectInput() {
    return GetSingleton()->pDirectInput;
}

LPDIRECTINPUTDEVICE8A BSInputManager::GetKeyboard() {
    return GetSingleton()->pKeyboard;
}

LPDIRECTINPUTDEVICE8A BSInputManager::GetMouse() {
    return GetSingleton()->pMouse;
}

// Very hacky
void BSInputManager::ToggleMouse(bool abDisable) {
    if (abDisable) {
        memset(&kCurrentMouseData, 0, sizeof(MouseData));
        memset(&kLastMouseData, 0, sizeof(MouseData));
    }
}

void BSInputManager::ToggleKeyboard(bool abDisable) {
    // Yea this is gonna kill me one day
    static const LPDIRECTINPUTDEVICE8A pSavedKeyboard = GetKeyboard();

    if (abDisable && pKeyboard) {
        pKeyboard = nullptr;
		memset(currKeyStates, 0, sizeof(currKeyStates));
		memset(lastKeyStates, 0, sizeof(lastKeyStates));
	}
    else if (!abDisable && !pKeyboard) {
		pKeyboard = pSavedKeyboard;
	}
}
// 0xA23C00
void BSInputManager::ChangeMouseCooperativeLevel(bool abBackground) {
    if (pMouse) {
        pMouse->Unacquire();
        pMouse->Release();

        LPDIRECTINPUTDEVICE8A pNewMouse;

        if (SUCCEEDED(pDirectInput->CreateDevice(GUID_SysMouse, &pNewMouse, 0))) {
            pMouse = pNewMouse;
            HWND pWindow = GetActiveWindow();
            if (abBackground) {
                _MESSAGE("[ BSInputManager::ChangeMouseCooperativeLevel ] Setting mouse to nonexclusive");
                pMouse->SetCooperativeLevel(pWindow, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
                uiFlags.SetBit(kFlag_BackgroundMouse, true);
            }
            else {
                _MESSAGE("[ BSInputManager::ChangeMouseCooperativeLevel ] Setting mouse to exclusive");
                pMouse->SetCooperativeLevel(pWindow, DISCL_FOREGROUND | DISCL_EXCLUSIVE);
                uiFlags.SetBit(kFlag_BackgroundMouse, false);
            }
            pMouse->SetDataFormat(BSInputManager::GetMouseDataFormat());
        }
        else {
            _MESSAGE("[ BSInputManager::ChangeMouseCooperativeLevel ] Failed to create mouse device");
        }
    }
}