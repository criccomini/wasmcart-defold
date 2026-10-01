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

#ifndef DM_TIME_H
#define DM_TIME_H

#include <stdint.h>
#include <dmsdk/dlib/time.h>

namespace dmTime
{
#if defined(DM_PLATFORM_WASMCART)
    /**
     * Advance the cart's VIRTUAL clock. A wasmcart is stepped by its host, so
     * the engine's timeline must be a function of how often wc_render() is
     * called rather than of wall-clock time. Called once per host frame; see
     * the comment in time_posix.cpp for why the wall clock is wrong here.
     * @param microseconds time to add to the virtual clock
     */
    void AdvanceVirtualTime(uint64_t microseconds);
#endif

    inline void BusyWait(uint32_t useconds) {
        uint64_t end = dmTime::GetMonotonicTime() + (uint64_t)useconds;
        while (dmTime::GetMonotonicTime() < end);
    }
}

#endif // DM_TIME_H
