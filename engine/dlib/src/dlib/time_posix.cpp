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

#include "time.h"

#include <time.h>
#include <unistd.h>

namespace dmTime
{
    void Sleep(uint32_t useconds)
    {
        usleep(useconds);
    }

    uint64_t GetTime()
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000U + ts.tv_nsec / 1000U;
    }

#if defined(DM_PLATFORM_WASMCART)
    // A CART IS STEPPED BY ITS HOST, NOT PACED BY A WALL CLOCK.
    //
    // The engine derives its frame delta from GetMonotonicTime(), so on a
    // wasmcart it measured REAL elapsed time between wc_render() calls. A host
    // stepping headless runs far faster than realtime, so 120 frames advanced
    // the clock by 0.026s instead of 2s - and every dt-driven system (timers,
    // physics, go.animate, tweens, particle simulation) sat still while
    // rendering and input looked perfectly fine.
    //
    // Fixing it by setting display.update_frequency does NOT work: that path
    // waits on the same clock and starves instead (measured: 3 script updates
    // in 2000 host frames).
    //
    // So the clock becomes VIRTUAL: it advances by exactly one frame's worth
    // of microseconds each time the host calls wc_render(). That makes the
    // engine's timeline a function of how often the cart is stepped, which is
    // the only definition that is correct for a cart - and it makes playback
    // deterministic, so the same input script produces the same frame on any
    // machine.
    static uint64_t g_VirtualTimeUs = 0;

    void AdvanceVirtualTime(uint64_t microseconds)
    {
        g_VirtualTimeUs += microseconds;
    }

    uint64_t GetMonotonicTime()
    {
        return g_VirtualTimeUs;
    }
#else
    uint64_t GetMonotonicTime()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t) ts.tv_sec * 1000000U + ts.tv_nsec / 1000U;
    }
#endif
}
