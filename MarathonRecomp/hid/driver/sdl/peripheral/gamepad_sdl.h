#pragma once

#include <hid/driver/sdl/peripheral/keyboard_sdl.h>
#include <hid/message/messages.h>
#include <hid/peripheral/gamepad.h>
#include <hid/hid.h>
#include <patches/pause_listener.h>
#include <SDL3/SDL.h>
#include <app.h>

namespace HID
{
    class GamepadSDL : public Gamepad
    {
    public:
        GamepadSDL() = default;

        explicit GamepadSDL(SDL_Gamepad* in_pSDLGamepad)
        {
            assert(in_pSDLGamepad);

            m_pContext = in_pSDLGamepad;

            if (!m_pContext)
                return;

            m_ID = SDL_GetJoystickID(SDL_GetGamepadJoystick(in_pSDLGamepad));
        }

        ~GamepadSDL()
        {
            SDL_CloseGamepad(GetContext<SDL_Gamepad>());
        }

        virtual std::string GetName() const override
        {
            if (const auto result = SDL_GetGamepadName(GetContext<SDL_Gamepad>()))
                return result;

            return Gamepad::GetName();
        }

        virtual PeripheralVendor GetVendor() const override
        {
            const auto pGamepad = GetContext<SDL_Gamepad>();
            if (!pGamepad)
                return PeripheralVendor::Xbox;

            switch (SDL_GetGamepadType(pGamepad))
            {
                case SDL_GAMEPAD_TYPE_PS3:
                case SDL_GAMEPAD_TYPE_PS4:
                case SDL_GAMEPAD_TYPE_PS5:
                    return PeripheralVendor::PlayStation;

                default:
                    break;
            }

            // Fallback for iOS GCController product names (e.g. DualSense Edge, Backbone One PlayStation Edition)
            if (const char* name = SDL_GetGamepadName(pGamepad))
            {
                std::string_view nameView(name);
                if (nameView.find("DualSense") != std::string_view::npos ||
                    nameView.find("DUALSHOCK") != std::string_view::npos ||
                    nameView.find("PlayStation") != std::string_view::npos ||
                    nameView.find("PS4") != std::string_view::npos ||
                    nameView.find("PS5") != std::string_view::npos)
                {
                    return PeripheralVendor::PlayStation;
                }
            }

            return PeripheralVendor::Xbox;
        }

        virtual XAMINPUT_GAMEPAD GetState() override
        {
            m_State = {};

            if (!HID::IsInputAllowed())
                return m_State;

            const auto pGamepad = GetContext<SDL_Gamepad>();
            if (!pGamepad)
                return m_State;

            m_State.sThumbLX = SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_LEFTX);
            m_State.sThumbLY = ~SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_LEFTY);

            m_State.sThumbRX = SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_RIGHTX);
            m_State.sThumbRY = ~SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_RIGHTY);

            // Clamp analog trigger range [0, 32767] before shifting so negative calibration jitter on MFi/Bluetooth
            // triggers never underflows into 0xFF (full press).
            const int leftTriggerRaw = std::clamp<int>(SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0, 32767);
            const int rightTriggerRaw = std::clamp<int>(SDL_GetGamepadAxis(pGamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, 32767);
            m_State.bLeftTrigger = static_cast<uint8_t>(leftTriggerRaw >> 7);
            m_State.bRightTrigger = static_cast<uint8_t>(rightTriggerRaw >> 7);

            const auto translate = [pGamepad](const auto& in_sdlInput, const auto& in_xamInput) -> int
            {
                return SDL_GetGamepadButton(pGamepad, in_sdlInput) << FirstBitLow(in_xamInput);
            };

            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT);

            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_START, XAMINPUT_GAMEPAD_START);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_TOUCHPAD, XAMINPUT_GAMEPAD_BACK);

            // Legacy MFi Extended Gamepads without a dedicated Options/Back button can use Guide or Misc1 as Back.
            if (!SDL_GamepadHasButton(pGamepad, SDL_GAMEPAD_BUTTON_BACK))
            {
                m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_MISC1, XAMINPUT_GAMEPAD_BACK);
                m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_GUIDE, XAMINPUT_GAMEPAD_BACK);
            }

            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_LEFT_STICK, XAMINPUT_GAMEPAD_LEFT_THUMB);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_RIGHT_STICK, XAMINPUT_GAMEPAD_RIGHT_THUMB);

            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER);

            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_SOUTH, XAMINPUT_GAMEPAD_A);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_EAST, XAMINPUT_GAMEPAD_B);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_WEST, XAMINPUT_GAMEPAD_X);
            m_State.wButtons |= translate(SDL_GAMEPAD_BUTTON_NORTH, XAMINPUT_GAMEPAD_Y);

            m_LastState = m_State;

            return m_State;
        }

        virtual bool SetVibration(const XAMINPUT_VIBRATION& in_rVibration, const uint32_t in_duration = 5000) override
        {
            m_LastVibration = in_rVibration;

            const auto pGamepad = GetContext<SDL_Gamepad>();
            if (!pGamepad)
                return false;

            const SDL_PropertiesID props = SDL_GetGamepadProperties(pGamepad);
            if (props && !SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
                return false;

            return SDL_RumbleGamepad(pGamepad, m_LastVibration.wLeftMotorSpeed, m_LastVibration.wRightMotorSpeed, in_duration);
        }

        virtual bool OnProcessMessage(IMessage& io_rMessage) override
        {
            if (io_rMessage.Is<MsgPeripheralAttached>())
            {
                MsgGetPeripheralUsers msgGetPeripheralUsers(this);
                GetManager()->ProcessMessage(msgGetPeripheralUsers);

                const auto pApp = App::s_pApp;

                if (!pApp)
                    return false;

                const auto pGame = App::s_pApp->GetGame();

                if (!pGame)
                    return false;

                for (const auto& rUser : msgGetPeripheralUsers.Users)
                {
                    // In the event that this gamepad is reconnected, this sets the
                    // gamepad LED to the colour associated with the character for
                    // this user.
                    if (const auto pPlayer = pGame->GetPlayer(rUser->ID))
                        ProcessMessage<MsgSetCharacterLED>(pPlayer->m_Name.c_str());
                }
            }
            else if (io_rMessage.Is<MsgPeripheralDetached>())
            {
                PauseListener::SignalPause();
            }
            else if (io_rMessage.Is<MsgSetCharacterLED>())
            {
                ProcessMessage<MsgSetLED>(static_cast<MsgSetCharacterLED*>(&io_rMessage)->Color);
            }
            else if (io_rMessage.Is<MsgSetLED>())
            {
                const auto pGamepad = GetContext<SDL_Gamepad>();
                if (pGamepad)
                {
                    const SDL_PropertiesID props = SDL_GetGamepadProperties(pGamepad);
                    if (!props || SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false))
                    {
                        const auto pMsgSetLED = static_cast<MsgSetLED*>(&io_rMessage);

                        const auto a = pMsgSetLED->Color.A() / 255.0f;
                        const auto r = uint8_t(pMsgSetLED->Color.R() * a);
                        const auto g = uint8_t(pMsgSetLED->Color.G() * a);
                        const auto b = uint8_t(pMsgSetLED->Color.B() * a);

                        SDL_SetGamepadLED(pGamepad, r, g, b);
                    }
                }
            }
            else if (io_rMessage.Is<MsgSetVibration>())
            {
                const auto pMsgSetVibration = static_cast<MsgSetVibration*>(&io_rMessage);

                SetVibration(pMsgSetVibration->Vibration, pMsgSetVibration->Duration);
            }

            return Gamepad::OnProcessMessage(io_rMessage);
        }
    };
}
