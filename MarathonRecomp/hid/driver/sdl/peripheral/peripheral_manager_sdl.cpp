#include "peripheral_manager_sdl.h"
#include <hid/driver/sdl/peripheral/gamepad_sdl.h>
#include <hid/driver/sdl/peripheral/keyboard_sdl.h>
#include <hid/driver/sdl/peripheral/mouse_sdl.h>
#include <hid/message/messages.h>
#include <hid/peripheral/peripheral_manager.h>
#include <hid/hid.h>
#include <os/logger.h>
#include <SDL3/SDL.h>
#include <ui/game_window.h>
#include <xxHashMap.h>

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/sandbox_ios.h>
#endif

namespace HID
{
    static PeripheralManagerSDL g_manager{};

    static bool SDL_EventFilter(void*, SDL_Event* in_pEvent)
    {
        switch (in_pEvent->type)
        {
            case SDL_EVENT_GAMEPAD_ADDED:
            {
                if (g_manager.Find(in_pEvent->gdevice.which))
                    break;

                auto* pad = SDL_OpenGamepad(in_pEvent->gdevice.which);
                if (!pad)
                {
                    LOGFN_ERROR("Failed to open gamepad (ID: {}): {}", in_pEvent->gdevice.which, SDL_GetError());
                    return true;
                }

                g_manager.Attach(std::make_shared<GamepadSDL>(pad));
                break;
            }

            case SDL_EVENT_GAMEPAD_REMOVED:
                g_manager.Detach(g_manager.Find(in_pEvent->gdevice.which));
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
#ifdef MARATHON_RECOMP_IOS
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
#endif
                g_manager.BroadcastMessage<MsgSetVibration>(XAMINPUT_VIBRATION{ 0, 0 });
                break;

            case SDL_EVENT_QUIT:
                g_manager.BroadcastMessage<MsgSetLED>(0, 0, 0);
                break;
        }

        return true;
    }

    bool PeripheralManagerSDL::Init()
    {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef MARATHON_RECOMP_IOS
        // Enable Apple GameController.framework (MFi, DualSense, DualShock 4, Xbox Wireless/Elite, Backbone)
        SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "1");
        // Prevent UIKit/SwiftUI focus system from intercepting B/Circle and Menu/Options buttons
        SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, "0");
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
        os::ios::ConfigureGameControllerSupport(true);
#else
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
#endif

        SDL_InitSubSystem(SDL_INIT_EVENTS);

        static bool s_eventWatchAdded = false;
        if (!s_eventWatchAdded)
        {
            SDL_AddEventWatch(SDL_EventFilter, nullptr);
            s_eventWatchAdded = true;
        }

        SDL_InitSubSystem(SDL_INIT_GAMEPAD);

        if (const auto mappings = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt"); mappings > 0)
            LOGFN("Loaded SDL gamepad mappings: {}", mappings);
#ifdef MARATHON_RECOMP_IOS
        else
        {
            const auto bundleMappingsPath = os::ios::GetBundleResourcePath() / "gamecontrollerdb.txt";
            if (!bundleMappingsPath.empty())
            {
                if (const auto bundleMappings = SDL_AddGamepadMappingsFromFile(bundleMappingsPath.c_str()); bundleMappings > 0)
                    LOGFN("Loaded bundled iOS SDL gamepad mappings: {}", bundleMappings);
            }
        }
#endif

        const bool baseInit = PeripheralManager::Init();

        // Attach any MFi / DualSense / Xbox gamepads already paired before SDL_AddEventWatch or app launch.
        int gamepadCount = 0;
        if (SDL_JoystickID* gamepadIds = SDL_GetGamepads(&gamepadCount))
        {
            for (int i = 0; i < gamepadCount; ++i)
            {
                if (g_manager.Find(gamepadIds[i]))
                    continue;

                if (auto* pad = SDL_OpenGamepad(gamepadIds[i]))
                    g_manager.Attach(std::make_shared<GamepadSDL>(pad));
            }
            SDL_free(gamepadIds);
        }

        return baseInit;
    }

    bool PeripheralManagerSDL::OnProcessMessage(IMessage& io_rMessage)
    {
        if (io_rMessage.Is<MsgRegisterKeyboard>())
        {
            auto pUser = GetUser(static_cast<MsgRegisterKeyboard*>(&io_rMessage)->UserID);

            if (!pUser)
                pUser = GetUser(0);

            ProcessMessage<MsgUnregisterKeyboard>();

            pUser->Attach(std::move(std::make_shared<KeyboardSDL>()));
#ifndef MARATHON_RECOMP_IOS
            // On iOS, single-finger screen touches synthesize SDL mouse state; avoid mapping touch-mouse clicks to gamepad inputs.
            pUser->Attach(std::move(std::make_shared<MouseSDL>()));
#endif
        }
        else if (io_rMessage.Is<MsgSetGamepadUser>())
        {
            const auto pMsgSetGamepadUser = static_cast<MsgSetGamepadUser*>(&io_rMessage);
            const auto pPeripheral = GetUser(pMsgSetGamepadUser->UserID)->Find(pMsgSetGamepadUser->PeripheralID);

            if (!pPeripheral || pPeripheral->GetType() != PeripheralType::Gamepad)
                return false;

            const auto pGamepad = static_cast<GamepadSDL*>(pPeripheral);

            SDL_SetGamepadPlayerIndex(pGamepad->GetContext<SDL_Gamepad>(), pMsgSetGamepadUser->UserID);
        }
        else if (io_rMessage.Is<MsgSetKeyboardUser>())
        {
            const auto userId = static_cast<MsgSetKeyboardUser*>(&io_rMessage)->UserID;

            ProcessMessage<MsgPushGamepads>(userId);
            ProcessMessage<MsgRegisterKeyboard>(userId);
        }

        return PeripheralManager::OnProcessMessage(io_rMessage);
    }

    PeripheralManager* GetManager()
    {
        return &g_manager;
    }
}
