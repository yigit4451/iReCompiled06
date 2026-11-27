#include "hid.h"
#include <atomic>
#include <ui/game_window.h>
#include <user/config.h>

namespace HID
{
    namespace
    {
        std::atomic<int32_t> g_activeTouchFingers{ 0 };
        std::atomic<uint32_t> g_touchSkipPulsePolls{ 0 };
        std::atomic<uint16_t> g_virtualGamepadButtons{ 0 };
    }

    void NotifyTouchSkipPressed()
    {
        g_activeTouchFingers.fetch_add(1, std::memory_order_relaxed);
        g_touchSkipPulsePolls.store(6, std::memory_order_relaxed);
    }

    void NotifyTouchSkipReleased()
    {
        int32_t cur = g_activeTouchFingers.load(std::memory_order_relaxed);
        while (cur > 0 && !g_activeTouchFingers.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed))
        {
        }
    }

    void SetVirtualGamepadButtons(uint16_t in_xamButtonMask)
    {
        g_virtualGamepadButtons.store(in_xamButtonMask, std::memory_order_relaxed);
    }

    uint16_t GetVirtualGamepadButtons()
    {
        return g_virtualGamepadButtons.load(std::memory_order_relaxed);
    }

    bool GetState(uint32_t in_userIndex, XAMINPUT_STATE* out_pState)
    {
        static uint32_t packetNumber{};

        if (!out_pState)
            return false;

        if (const auto pUser = GetManager()->GetUser(in_userIndex))
        {
            XAMINPUT_STATE state
            {
                .dwPacketNumber = packetNumber++,
                .Gamepad = pUser->GetState()
            };

#ifdef MARATHON_RECOMP_IOS
            if (in_userIndex == 0 && IsInputAllowed())
            {
                state.Gamepad.wButtons |= g_virtualGamepadButtons.load(std::memory_order_relaxed);

                const bool fingerHeld = g_activeTouchFingers.load(std::memory_order_relaxed) > 0;
                uint32_t pulse = g_touchSkipPulsePolls.load(std::memory_order_relaxed);
                if (pulse > 0)
                    g_touchSkipPulsePolls.compare_exchange_strong(pulse, pulse - 1, std::memory_order_relaxed);

                if (fingerHeld || pulse > 0)
                {
                    state.Gamepad.wButtons |= (XAMINPUT_GAMEPAD_START | XAMINPUT_GAMEPAD_A);
                }
            }
#endif

            *out_pState = state;

            return true;
        }

        return false;
    }

    bool SetState(uint32_t in_userIndex, XAMINPUT_VIBRATION* in_pVibration)
    {
        if (!in_pVibration)
            return false;

        if (const auto pUser = GetManager()->GetUser(in_userIndex))
        {
            pUser->SetVibration(*in_pVibration);
            return true;
        }

        return false;
    }

    bool GetCapabilities(uint32_t in_userIndex, XAMINPUT_CAPABILITIES* out_pCapabilities)
    {
        if (!out_pCapabilities)
            return false;

        if (const auto pUser = GetManager()->GetUser(in_userIndex))
        {
            XAMINPUT_CAPABILITIES capabilities
            {
                .Type = XAMINPUT_DEVTYPE_GAMEPAD,
                .SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD,
                .Flags = 0,
                .Gamepad = pUser->GetState(),
                .Vibration = pUser->GetVibration()
            };

            *out_pCapabilities = capabilities;

            return true;
        }

        return false;
    }

    bool IsInputAllowed()
    {
        return GameWindow::s_isFocused || Config::AllowBackgroundInput;
    }
}
