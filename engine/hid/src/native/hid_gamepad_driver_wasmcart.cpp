// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// The gamepad driver for wasmcart carts.
//
// A cart has no joystick enumeration API and receives no connect/disconnect
// events: the host refreshes a small block of shared memory before each frame
// and the cart reads it. So this driver polls, exactly like the keyboard path
// in platform_window_wasmcart.cpp does, and derives connect/disconnect from
// the block's own m_Connected flag.
//
// The packet it publishes is the CANONICAL SDL-style layout
// (GAMEPAD_MAPPED_*), and GetGamepadMappingSupport reports
// GAMEPAD_MAPPING_SUPPORT_AUTOMATIC. That is what makes the standard action
// names work without shipping a device row in gamecontrollerdb.txt: input.cpp
// installs CreateAutomaticGamepadConfig for any gamepad whose driver claims
// AUTOMATIC, and that config is what turns mapped button A into
// "gamepad_rpad_down" and the hat into "gamepad_lpad_*".

#include <assert.h>
#include <string.h>

#include <dlib/array.h>
#include <dlib/dstrings.h>
#include <dlib/log.h>
#include <dlib/math.h>

#include <platform/window.hpp>

#include "hid.h"
#include "hid_wasmcart.h"
#include "hid_private.h"
#include "hid_native_private.h"

#include "platform_window_wasmcart.h"

namespace dmHID
{
    // The host reports every analog axis as int16 (ABI v4): sticks are
    // full-range and triggers are 0..32767, never negative.
    // Defold wants every mapped axis in -1..1, TRIGGERS INCLUDED: the
    // automatic gamepad config registers LTRIGGER/RTRIGGER with scale set
    // (input.cpp SetAutomaticGamepadAxis), and a scaled axis is read as
    // (v + 1) * 0.5. So a trigger has to REST AT -1, not at 0.
    //
    // Handing it 0..1 instead put a released trigger at 0, which became 0.5
    // after that transform -- over the threshold, so both triggers read as
    // HELD from the first frame with nothing touched. planetoid binds
    // GAMEPAD_LTRIGGER to "left" and GAMEPAD_RTRIGGER to "right", so both
    // fired every frame and cancelled out, and the arrow keys could only
    // raise an already-0.5 action to 1.0 (an action takes the max), so the
    // ship never moved. It looked like dead input, not a stuck axis.
    //
    // 2/32767 then -1 maps rest 0 to -1 and full 32767 to +1, which is what
    // gamepad_darwin.mm does for the same axes (value * 2.0f - 1.0f).
    static const float WASMCART_AXIS_SCALE    = 1.0f / 32767.0f;
    static const float WASMCART_TRIGGER_SCALE = 2.0f / 32767.0f;

    // A stick pushed past this counts as a d-pad press for the hat. Without a
    // hat the automatic config's gamepad_lpad_* bindings never fire, and a
    // cart host that maps its d-pad onto the pad's direction bits would look
    // like a dead d-pad. The wasmcart pad reports BOTH, so the hat is built
    // from the direction BITS (exact) rather than from the stick.
    struct WasmcartGamepadDevice
    {
        Gamepad* m_Gamepad;
        uint32_t m_Index;
    };

    struct WasmcartGamepadDriver : GamepadDriver
    {
        HContext                          m_HidContext;
        dmArray<WasmcartGamepadDevice>    m_Devices;
    };

    static WasmcartGamepadDriver* g_WasmcartGamepadDriver = 0;

    static WasmcartGamepadDevice* FindDevice(WasmcartGamepadDriver* driver, Gamepad* gamepad)
    {
        for (uint32_t i = 0; i < driver->m_Devices.Size(); ++i)
        {
            if (driver->m_Devices[i].m_Gamepad == gamepad)
            {
                return &driver->m_Devices[i];
            }
        }
        return 0;
    }

    bool GetWasmcartPadIndex(uint32_t gamepad_index, uint32_t* pad_index)
    {
        WasmcartGamepadDriver* driver = g_WasmcartGamepadDriver;
        if (!driver || gamepad_index >= MAX_GAMEPAD_COUNT)
        {
            return false;
        }

        // CreateGamepad allocates the first free HID entry. Its index can
        // differ from the host slot when pads connect out of order.
        Gamepad* gamepad = &driver->m_HidContext->m_Gamepads[gamepad_index];
        WasmcartGamepadDevice* device = FindDevice(driver, gamepad);
        if (!device || !gamepad->m_Connected)
        {
            return false;
        }
        *pad_index = device->m_Index;
        return true;
    }

    static bool HasDeviceForIndex(WasmcartGamepadDriver* driver, uint32_t index)
    {
        for (uint32_t i = 0; i < driver->m_Devices.Size(); ++i)
        {
            if (driver->m_Devices[i].m_Index == index)
            {
                return true;
            }
        }
        return false;
    }

    static void RemoveDeviceForIndex(WasmcartGamepadDriver* driver, uint32_t index)
    {
        for (uint32_t i = 0; i < driver->m_Devices.Size(); ++i)
        {
            if (driver->m_Devices[i].m_Index == index)
            {
                SetGamepadConnectionStatus(driver->m_HidContext, driver->m_Devices[i].m_Gamepad, false);
                ReleaseGamepad(driver->m_HidContext, driver->m_Devices[i].m_Gamepad);
                driver->m_Devices.EraseSwap(i);
                return;
            }
        }
    }

    // Polled once a frame in place of the connect/disconnect events a cart
    // never receives.
    static void WasmcartGamepadDriverDetectDevices(HContext context, GamepadDriver* driver)
    {
        WasmcartGamepadDriver* wc_driver = (WasmcartGamepadDriver*) driver;

        for (uint32_t i = 0; i < WASMCART_PAD_COUNT; ++i)
        {
            dmPlatform::WasmcartPad pad;
            bool present = dmPlatform::WasmcartGetPad(i, &pad) && pad.m_Connected;
            bool known   = HasDeviceForIndex(wc_driver, i);

            if (present && !known)
            {
                Gamepad* gp = CreateGamepad(context, driver);
                if (gp == 0)
                {
                    continue;
                }
                gp->m_ButtonCount = GAMEPAD_MAPPED_BUTTON_COUNT;
                gp->m_AxisCount   = GAMEPAD_MAPPED_AXIS_COUNT;
                gp->m_HatCount    = 1;

                WasmcartGamepadDevice device;
                device.m_Gamepad = gp;
                device.m_Index   = i;
                if (wc_driver->m_Devices.Full())
                {
                    wc_driver->m_Devices.OffsetCapacity(1);
                }
                wc_driver->m_Devices.Push(device);

                SetGamepadConnectionStatus(context, gp, true);
            }
            else if (!present && known)
            {
                RemoveDeviceForIndex(wc_driver, i);
            }
        }
    }

    static void SetButton(GamepadPacket* packet, uint32_t button, bool down)
    {
        if (down)
        {
            packet->m_Buttons[button / 32] |= 1 << (button % 32);
        }
        else
        {
            packet->m_Buttons[button / 32] &= ~(1 << (button % 32));
        }
    }

    static void WasmcartGamepadDriverUpdate(HContext context, GamepadDriver* driver, Gamepad* gamepad)
    {
        WasmcartGamepadDriver* wc_driver = (WasmcartGamepadDriver*) driver;
        WasmcartGamepadDevice* device    = FindDevice(wc_driver, gamepad);
        if (device == 0)
        {
            return;
        }

        dmPlatform::WasmcartPad pad;
        if (!dmPlatform::WasmcartGetPad(device->m_Index, &pad))
        {
            return;
        }

        GamepadPacket& packet = gamepad->m_Packet;

        packet.m_Axis[GAMEPAD_MAPPED_AXIS_LEFT_X]  = pad.m_LeftX  * WASMCART_AXIS_SCALE;
        // The host's Y axes point DOWN (screen convention); Defold's mapped
        // axes point down as well, so these pass through unnegated. Getting
        // this backwards is invisible in a button test and shows up only as a
        // game walking the wrong way.
        packet.m_Axis[GAMEPAD_MAPPED_AXIS_LEFT_Y]  = pad.m_LeftY  * WASMCART_AXIS_SCALE;
        packet.m_Axis[GAMEPAD_MAPPED_AXIS_RIGHT_X] = pad.m_RightX * WASMCART_AXIS_SCALE;
        packet.m_Axis[GAMEPAD_MAPPED_AXIS_RIGHT_Y] = pad.m_RightY * WASMCART_AXIS_SCALE;
        packet.m_Axis[GAMEPAD_MAPPED_AXIS_LEFT_TRIGGER]  = pad.m_LeftTrigger  * WASMCART_TRIGGER_SCALE - 1.0f;
        packet.m_Axis[GAMEPAD_MAPPED_AXIS_RIGHT_TRIGGER] = pad.m_RightTrigger * WASMCART_TRIGGER_SCALE - 1.0f;

        // uint32, not uint16: ABI v4 widened m_Buttons and put GUIDE at bit
        // 14 through TOUCHPAD at bit 20. A uint16 local truncates every bit
        // above 15, which would drop MISC1, the paddles and the touchpad
        // silently -- they would simply never fire.
        const uint32_t b = pad.m_Buttons;

        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_A,              (b & WASMCART_BTN_A) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_B,              (b & WASMCART_BTN_B) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_X,              (b & WASMCART_BTN_X) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_Y,              (b & WASMCART_BTN_Y) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_LEFT_SHOULDER,  (b & WASMCART_BTN_L) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_RIGHT_SHOULDER, (b & WASMCART_BTN_R) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_START,          (b & WASMCART_BTN_START) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_BACK,           (b & WASMCART_BTN_SELECT) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_LEFT_THUMB,     (b & WASMCART_BTN_L3) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_RIGHT_THUMB,    (b & WASMCART_BTN_R3) != 0);
        // Real as of ABI v4, which added both. They were hardcoded false
        // because the pad had no bits for them, not because Defold lacks the
        // mapped buttons. MISC1 is the share/capture/microphone key, whose
        // closest Defold equivalent is CAPTURE.
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_GUIDE,   (b & WASMCART_BTN_GUIDE) != 0);
        SetButton(&packet, GAMEPAD_MAPPED_BUTTON_CAPTURE, (b & WASMCART_BTN_MISC1) != 0);

        // PADDLE1-4 and TOUCHPAD have no GAMEPAD_MAPPED_BUTTON_* equivalent,
        // so they stay unmapped rather than being aliased onto something
        // else. A game wanting them reads the pad through its own binding.

        // The d-pad is a HAT in the automatic config, and the mask bits are
        // the classic clockwise-from-up order (1 up, 2 right, 4 down, 8 left).
        uint8_t hat = 0;
        if (b & WASMCART_BTN_UP)    hat |= 0x1;
        if (b & WASMCART_BTN_RIGHT) hat |= 0x2;
        if (b & WASMCART_BTN_DOWN)  hat |= 0x4;
        if (b & WASMCART_BTN_LEFT)  hat |= 0x8;
        packet.m_Hat[0] = hat;

        gamepad->m_ButtonCount = GAMEPAD_MAPPED_BUTTON_COUNT;
        gamepad->m_AxisCount   = GAMEPAD_MAPPED_AXIS_COUNT;
        gamepad->m_HatCount    = 1;
    }

    static void WasmcartGamepadDriverGetGamepadDeviceName(HContext context, GamepadDriver* driver, Gamepad* gamepad, char name[MAX_GAMEPAD_NAME_LENGTH])
    {
        WasmcartGamepadDriver* wc_driver = (WasmcartGamepadDriver*) driver;
        WasmcartGamepadDevice* device    = FindDevice(wc_driver, gamepad);

        const char* host_name = device ? dmPlatform::WasmcartGetPadName(device->m_Index) : 0;
        dmStrlCpy(name, host_name ? host_name : "wasmcart gamepad", MAX_GAMEPAD_NAME_LENGTH);
    }

    static bool WasmcartGamepadDriverGetGamepadDeviceGuid(HContext context, GamepadDriver* driver, Gamepad* gamepad, GamepadGuid* guid)
    {
        // The host does not report a device GUID, so there is nothing to look
        // up in the physical database. Reporting false here (rather than a
        // made-up GUID) is what keeps input.cpp on the automatic config.
        return false;
    }

    static uint32_t WasmcartGamepadDriverGetGamepadMappingSupport(HContext context, GamepadDriver* driver, Gamepad* gamepad)
    {
        return GAMEPAD_MAPPING_SUPPORT_AUTOMATIC;
    }

    static bool WasmcartGamepadDriverInitialize(HContext context, GamepadDriver* driver)
    {
        return true;
    }

    static void WasmcartGamepadDriverDestroy(HContext context, GamepadDriver* driver)
    {
        WasmcartGamepadDriver* wc_driver = (WasmcartGamepadDriver*) driver;
        assert(g_WasmcartGamepadDriver == wc_driver);
        delete wc_driver;
        g_WasmcartGamepadDriver = 0;
    }

    GamepadDriver* CreateGamepadDriverWasmcart(HContext context)
    {
        WasmcartGamepadDriver* driver = new WasmcartGamepadDriver();

        driver->m_Initialize               = WasmcartGamepadDriverInitialize;
        driver->m_Destroy                  = WasmcartGamepadDriverDestroy;
        driver->m_Update                   = WasmcartGamepadDriverUpdate;
        driver->m_DetectDevices            = WasmcartGamepadDriverDetectDevices;
        driver->m_GetGamepadDeviceName     = WasmcartGamepadDriverGetGamepadDeviceName;
        driver->m_GetGamepadDeviceGuid     = WasmcartGamepadDriverGetGamepadDeviceGuid;
        driver->m_GetGamepadMappingSupport = WasmcartGamepadDriverGetGamepadMappingSupport;
        driver->m_SetGamepadMapping        = 0;

        assert(g_WasmcartGamepadDriver == 0);
        g_WasmcartGamepadDriver               = driver;
        g_WasmcartGamepadDriver->m_HidContext = context;

        return driver;
    }
}
