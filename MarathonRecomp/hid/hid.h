#pragma once

#include <hid/peripheral/peripheral_manager.h>
#include <xbox.h>

namespace HID
{
    extern PeripheralManager* GetManager();

    bool GetState(uint32_t in_userIndex, XAMINPUT_STATE* out_pState);
    bool SetState(uint32_t in_userIndex, XAMINPUT_VIBRATION* in_pVibration);
    bool GetCapabilities(uint32_t in_userIndex, XAMINPUT_CAPABILITIES* out_pCapabilities);

    bool IsInputAllowed();
    void NotifyTouchSkipPressed();
    void NotifyTouchSkipReleased();
    void SetVirtualGamepadButtons(uint16_t in_xamButtonMask);
    uint16_t GetVirtualGamepadButtons();
}
