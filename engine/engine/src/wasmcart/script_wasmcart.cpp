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

// Lua rumble API for wasmcart scripts. No Lua state or device is retained.

#include <assert.h>
#include <stdint.h>
#include <hid_wasmcart.h>
#include <platform_window_wasmcart.h>

extern "C"
{
#include <lua/lua.h>
#include <lua/lauxlib.h>
}

namespace dmEngine
{
    static bool GetPadIndex(lua_State* L, uint32_t* pad_index)
    {
        const lua_Number index = luaL_checknumber(L, 1);
        // Check before converting: NaN, infinity and negative values must
        // never reach a floating-point to unsigned integer conversion.
        if (!(index >= 0 && index <= UINT32_MAX))
        {
            return false;
        }
        const uint32_t gamepad = (uint32_t) index;
        return index == gamepad && dmHID::GetWasmcartPadIndex(gamepad, pad_index);
    }

    static int PadHasRumble(lua_State* L)
    {
        uint32_t pad_index;
        lua_pushboolean(L, GetPadIndex(L, &pad_index) && dmPlatform::WasmcartPadHasRumble(pad_index));
        return 1;
    }

    static int PadRumble(lua_State* L)
    {
        uint32_t pad_index;
        const bool connected = GetPadIndex(L, &pad_index);
        const lua_Number low = luaL_checknumber(L, 2);
        const lua_Number high = luaL_checknumber(L, 3);
        const lua_Number duration_ms = luaL_checknumber(L, 4);
        luaL_argcheck(L, low >= 0 && low <= 1, 2, "intensity must be between 0 and 1");
        luaL_argcheck(L, high >= 0 && high <= 1, 3, "intensity must be between 0 and 1");
        luaL_argcheck(L, duration_ms >= 0 && duration_ms <= UINT32_MAX, 4, "duration must be non-negative milliseconds within uint32 range");
        if (connected)
        {
            const uint32_t duration = (uint32_t) (duration_ms > 5000 ? 5000 : duration_ms);
            dmPlatform::WasmcartPadRumble(pad_index, (float) low, (float) high, duration);
        }
        return 0;
    }

    static int PadRumbleStop(lua_State* L)
    {
        uint32_t pad_index;
        if (GetPadIndex(L, &pad_index))
        {
            dmPlatform::WasmcartPadRumbleStop(pad_index);
        }
        return 0;
    }

    static const luaL_Reg WASMCART_METHODS[] =
    {
        {"pad_has_rumble", PadHasRumble},
        {"pad_rumble", PadRumble},
        {"pad_rumble_stop", PadRumbleStop},
        {0, 0}
    };

    void ScriptWasmcartInitialize(lua_State* L)
    {
        const int top = lua_gettop(L);
        (void)top;
        luaL_register(L, "wasmcart", WASMCART_METHODS);
        lua_pop(L, 1);
        assert(top == lua_gettop(L));
    }
}
