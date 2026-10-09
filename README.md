# wasmcart-defold

**Write games in Defold. Ship them as wasmcart cartridges.**

The [Defold](https://defold.com) engine compiled to standalone WebAssembly as a
[wasmcart](https://github.com/wasmcart/wasmcart) runtime.

[wasmcart](https://github.com/wasmcart/wasmcart) is a virtual cartridge format:
a `.wasc` file holds a WebAssembly program plus its assets, and runs unmodified
on any host that implements the ABI. No installer, no platform build, no
browser.

So the engine is built **once** into `dmengine_wasmcart.wasm`, and any Defold
game becomes a cart by packing its bob output beside it. That one `.wasc` then
runs everywhere a wasmcart host exists:

- [wasmcart](https://github.com/wasmcart/wasmcart) - the JS reference host,
  Node and browsers
- [wasmcart-native](https://github.com/wasmcart/wasmcart-native) - a standalone
  desktop player (libnode/V8, SDL2, EGL)
- [wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro) - a
  RetroArch core, so carts run anywhere RetroArch does
- [wasmcart-android](https://github.com/wasmcart/wasmcart-android) - a
  standalone Android player

This is Defold without a browser. Defold's own HTML5 export needs a DOM, a
WebGL canvas and JS glue; this is standalone WASM with none of that, which is
why it reaches hosts Defold has no export for.

Ready-made carts and worked projects are in
[wasmcart-defold-examples](https://github.com/wasmcart/wasmcart-defold-examples):
five complete games plus the fixtures that exercise the engine surface.

Upstream Defold is **vendored, not forked** (see
[How this repo is organised](#how-this-repo-is-organised)). Upstream's own
README is kept as [`docs/upstream/README_DEFOLD.md`](docs/upstream/README_DEFOLD.md).

## What works

| | |
| --- | --- |
| Engine boots as a cart | yes |
| Lua `init` / `update` / `on_input` | yes |
| Sprites, fonts, tilemaps, GUI, labels | yes |
| Input: keyboard, mouse, gamepad | yes |
| Sound | yes, measured against a 440 Hz source |
| Frame delta (`dt`), timers, `go.animate` | yes, exactly 1/60 per host frame |
| Physics: gravity, contacts, `collision_response` | yes |
| `sys.save` / `sys.load` / `sys.exists` | yes, persists across sessions |
| `resource.create_texture` / `set_texture` | yes |
| Particle FX, collection proxies, buffers | yes |

Verified on both wasmcart hosts (the JS reference host and `wasmcart-native`),
because each hides bugs the other exposes.

Known limits, both Defold's rather than this port's:

- A **sprite** cannot be pointed at a runtime texture. `comp_sprite.cpp`
  implements `PROP_TEXTURE` in the get path only, so `texture0` is read-only
  there; `comp_mesh.cpp` has the setter, so use a mesh component.
- `sprite.set_image` does not exist in this engine build.

## Building the runtime

Requires a Linux host with the Defold build prerequisites (see
[`docs/README_BUILD.md`](docs/README_BUILD.md), upstream's own instructions).

```bash
git remote add origin <this repo>   # see below: the build shells out to git
./scripts/build.py shell            # sets DEFOLD_HOME, DYNAMO_HOME, JAVA_HOME, PATH
./scripts/build.py --platform=wasm-web --skip-tests build_engine -- \
    --skip-build-tests --with-wasmcart
# -> tmp/dynamo_home/bin/wasm-web/dmengine_wasmcart.wasm
```

Two prerequisites that are easy to miss because upstream does not track either,
both found by building this repo from scratch:

- **The tree must be a git repo with an `origin` remote.** `build.py` runs
  `git remote get-url origin` and aborts with `No such remote 'origin'` if there
  is none. A plain archive extract will not build.
- **`dmenv.sh` is generated, not committed.** `./scripts/build.py shell` sets the
  same variables; `dmenv.sh` is only a convenience for re-entering that
  environment in a new terminal.

**`scripts/build.py` can print `Done` and exit 0 after a fatal compile error**,
leaving the previous `.wasm` in place so the next cart you build silently tests
the old engine. Check the artifact's mtime, not the exit code.

## Making a game

Build the content with bob, then pack it with the runtime:

```bash
java -jar tmp/dynamo_home/share/java/bob-light.jar --root <game> \
    --platform wasm-web --archive --use-uncompressed-lua-source build

node wasmcart-pack.js --wasm dmengine_wasmcart.wasm --assets <staged> \
    --name "My Game" --width 960 --height 540 --output mygame.wasc
```

Three bob flags are load-bearing and each fails without naming itself:

- **`--archive`** - without it bob writes no `game.arci/.arcd/.dmanifest` and
  the cart reports `Unable to load bootstrap data`.
- **`--platform wasm-web`** - without it bob emits Lua bytecode for the host
  architecture and the cart fails with `bad header in precompiled chunk`.
- **`--use-uncompressed-lua-source`** - bob's archive writer errors when
  compiling Lua to bytecode for this target.

A manifest `width`/`height` is the cart author stating the size the cart wants,
and the runtime adopts it as its resolution.

Worked examples, a `build-cart.sh` that encodes all of the above, and a
migration script for pre-1.13 projects live in
[wasmcart-defold-examples](https://github.com/wasmcart/wasmcart-defold-examples),
which carries twelve carts including five complete games.

## Loading content in parts

Cart builds support `liveupdate.add_mount(name, "zip:parts/area1.zip", priority, cb)`.
The path names a file inside the cart's assets directory. Use a relative path
with directories intact; `zip://` introduces a URI host and is not supported.
No download or writable filesystem is needed. Add mounts again each session.

Previously the ZIP provider called `zip_open` through stdio, so it could not
open cart assets. The cart ZIP backend now uses `wc_asset_size` and
`wc_load_asset`. It owns one buffer per mounted ZIP and frees it on
`liveupdate.remove_mount`, failed mounting, or engine shutdown. The ZIP reader
borrows the buffer. The base `game.arcd` stays resident, as before.

### Build and pack

Follow Defold's [excluded collection proxy workflow](https://defold.com/manuals/live-update/).
Keep the bootstrap collection, loader script and UI in the base game. Put each
area behind a collection proxy and tick **Exclude**. A `.collectionproxy`
file looks like this:

```text
collection: "/areas/area1.collection"
exclude: true
```

Resources also referenced by boot content remain in `game.arcd`. Keep large
textures and sounds out of boot atlases and other boot dependencies. A proxy
that loads later but has Exclude off still puts its content in the base archive.

In `game.project`, select a ZIP publisher:

```ini
[liveupdate]
settings = /liveupdate.settings
```

Create `liveupdate.settings` in the game project:

```ini
[liveupdate]
mode = Zip
zip-filepath = build/liveupdate
zip-filename = all.zip
```

Run these commands from the game project. `PORT` points to this port's checkout;
`bob.jar` and `dmengine_wasmcart.wasm` must already be available. Use the runtime
built from this change. Compile the base and the parts together on each build.

```sh
PORT=/path/to/wasmcart-defold
java -jar bob.jar --root . --platform wasm-web --archive \
    --use-uncompressed-lua-source --liveupdate yes build

mkdir -p staged/parts
cp build/default/game.arci build/default/game.arcd \
    build/default/game.dmanifest build/default/game.projectc staged/
python3 "$PORT/scripts/split_liveupdate.py" build/default/game.graph.json \
    build/liveupdate/all.zip staged/parts \
    area1=/areas/area1.collectionc area2=/areas/area2.collectionc

wasmcart pack --wasm dmengine_wasmcart.wasm --assets staged \
    --name "My Game" --width 960 --height 540 --output mygame.wasc
```

Bob publishes all excluded resources into one ZIP. The splitter walks each
compiled collection's dependencies in `game.graph.json`. It keeps bob's resource
headers, digest filenames and `liveupdate.game.dmanifest`. Shared excluded
resources go in every part that needs them; base resources stay in the base.
Give every part a distinct name. For one small part, copy `all.zip` to
`staged/parts/area1.zip` instead. Do not put `all.zip` in staging as well as the
split parts, or include the ZIPs as Defold custom resources.

### Mount, load and unload

For a proxy named `area1` on the loader's game object:

```lua
local function mounted(self, name, uri, result)
    assert(result == liveupdate.LIVEUPDATE_OK, "mount failed: " .. uri)
    msg.post("#area1", "async_load")
end

function init(self)
    local result = liveupdate.add_mount("area1", "zip:parts/area1.zip", 10, mounted)
    assert(result == liveupdate.LIVEUPDATE_OK) -- request accepted
end

function on_message(self, message_id, message, sender)
    if message_id == hash("proxy_loaded") then
        msg.post(sender, "init")
        msg.post(sender, "enable")
    elseif message_id == hash("leave_area1") then
        msg.post("#area1", "disable")
        msg.post("#area1", "final")
        msg.post("#area1", "unload")
    elseif message_id == hash("proxy_unloaded") then
        assert(liveupdate.remove_mount("area1") == liveupdate.LIVEUPDATE_OK)
    end
end
```

Wait for the mount callback before loading the proxy. Wait for
`proxy_unloaded` before removing its mount. Loaded textures and other resources
have their own lifetimes; removing a mount frees the ZIP, not those resources.
Release dynamic resource handles too. Shared resources still in use stay loaded.
Use a distinct mount name for each live part. Larger priorities win resource
lookups. A duplicate name fails and releases the new ZIP; it leaves the old
mount intact. To revisit an area, add its mount and load its proxy again.

Mounting reads a whole ZIP in one operation. The callback API does not make
that read incremental: this cart build processes it on the main thread.
On Couchmix, keep each ZIP loaded during play well below 150 MiB to avoid the
2 s heartbeat timeout. Each asset must fit the 256 MiB cap. Budget the base
archive, mounted ZIPs, loaded resources and temporary extraction buffers within
the game's roughly 1 GiB limit. Mount only the current and upcoming parts;
unload and unmount old ones.

### Local checks

```sh
# From this checkout; no engine build or downloaded dependencies needed:
bash scripts/test_cart_mounts.sh
```

The host test uses the real ZIP reader and mocked cart imports. It checks
stored and deflated ZIPs, a bob Live Update ZIP, entry reads without reloading,
independent buffers, repeated add/remove, duplicate names, missing assets,
failed/short reads, allocation failure and corrupt ZIPs. It tracks owned buffer
bytes and runs with address and undefined-behavior sanitizers. The script also
checks C++ syntax and the splitter's dependency and manifest handling.

These checks do not run the full resource provider, Lua callbacks, collection
proxies or the wasm host. CI must build the runtime; a cart on Couchmix must
verify proxy loads, unloads, memory and heartbeat timing.

## Save capacity

`sys.save` and `sys.load` use the host's persistent save region. To raise its
capacity, add this to `game.project` and rebuild the cart:

```ini
[wasmcart]
save_size = 1048576
```

`wasmcart.save_size` is the total region size in bytes, including the file
table. The default is **132144 bytes**, the original eight 16 KiB slots and
their metadata. The engine clamps values to 132144–4194304 bytes and logs the
chosen total and per-file limit. Invalid integers use the default.

The region holds at most eight files. Each has a 128-byte name field, a
4-byte length, and an equal share of the payload space. The per-file limit is
`floor(((save_size - 16) / 8 - 132) / 4) * 4` bytes. At 1 MiB this is 130936
bytes; at 4 MiB it is 524152 bytes. Names must fit in 127 bytes. A file cannot
borrow an unused slot's space.

Couchmix allows **4 MiB per game per profile** and refuses larger regions.
The engine allocates the chosen region once in the cart's wasm memory, before
the host restores saves. That allocation stays for the session and counts
against Couchmix's roughly 1 GiB game memory budget. Save serialization also
uses a temporary buffer; raising the capacity does not reserve 4 MiB for
games that keep the default.

The default keeps the original save format. Larger regions use a versioned
header and migrate restored version 1 or smaller version 2 slots in place,
without allocating a second block. The host must copy the smaller saved block
into the larger region and zero-fill the remainder for this migration to run.

**Couchmix's current runner rejects saved files whose length differs from the
region size.** Capacity increases for games with existing saves need a runner
change to restore smaller blocks with zero padding. The engine cannot migrate
bytes the host refuses to restore. Keeping the default loads old saves without
a runner change.

Keep the same capacity or raise it in later releases. Lowering it can lose
saved data on hosts that copy a truncated prefix; the engine refuses access
to a restored layout with larger slots.

The block logic has a standalone host test. No Defold build is needed:

```bash
c++ -std=c++11 -Wall -Wextra -Werror \
    engine/platform/src/test/test_wasmcart_save.cpp -o /tmp/test_wasmcart_save
/tmp/test_wasmcart_save
```

## Rumble

The wasmcart engine exposes these Lua functions:

```lua
wasmcart.pad_has_rumble(action.gamepad) -- boolean
wasmcart.pad_rumble(action.gamepad, 1.0, 0.0, 300) -- low, high, milliseconds
wasmcart.pad_rumble_stop(action.gamepad)
```

Pass the index from `action.gamepad`. The driver maps it to the current
host pad slot, including pads that connect out of order.

Motor strengths must be numbers from 0 to 1. Duration must be non-negative
and fit in a uint32. It is truncated to whole milliseconds and capped at
5000 ms. Re-arm each frame for sustained rumble, then stop on release.
The two motor functions return no values.

Absent or invalid numeric gamepad indices return false from
`pad_has_rumble`; the motor functions do nothing. Devices without motors
and hosts that stub missing imports with zero or -1 behave the same way.
Invalid strengths or durations raise a Lua argument error.

## Indexed meshes

The `indexed-mesh` branch adds a runtime `indices` property to mesh components.
It uses the existing buffer resource type. Upstream bob 1.13.2 can compile the
content; no mesh descriptor or proto changes are required.

```lua
local b = buffer.create(6, {{name = hash("index"), type = buffer.VALUE_TYPE_UINT16, count = 1}})
local stream = buffer.get_stream(b, "index")
local data = {0, 1, 2, 0, 2, 3} -- four vertices, two triangles; zero-based indices
for i, value in ipairs(data) do stream[i] = value end
local indices = resource.create_buffer("/runtime-indices.bufferc", {buffer = b})
go.set("#mesh", "indices", indices)
-- The component holds its own resource reference after this release.
resource.release(indices)
```

The material must use local vertex space. The index buffer must have one
scalar uint16 or uint32 stream. The stream name is unrestricted. The adapter
must support the type, and every index must be below the vertex count.
The draw uses the index count and a zero byte offset. Non-indexed components
keep DrawArrays; world-space batching keeps its original path.

`go.get("#mesh", "indices")` returns the buffer resource hash, or `hash("")`
when unset. Older engines report an unknown property; use `pcall(go.get, url,
"indices")` to check support. Clear with `go.set(url, "indices", hash(""))`.
If you switched to unique vertices, restore the expanded vertices before
resuming a triangle-list draw.

Invalid assignments leave the old property intact. Buffer edits and
`resource.set_buffer` replacements trigger validation and upload. Invalid
edited data suppresses drawing until repaired. Empty index buffers draw
nothing. Each component owns its GPU index buffer and holds the resource
reference until clear, replacement or destruction. Vertex buffers still share
the local-space resource cache. Material/resource reloads retain the actual
vertex-buffer reference and refuse indexed world-space rendering.

The GLES3 adapter now reports core uint32 index support without requiring an
extension string. Leave the all-ones index unused in WebGL2 content because
that backend reserves it for primitive restart.

The host helper test uses real dmBuffer storage and ASan/UBSan. It requires an
existing C++ compiler, protoc and Python environment with protobuf:

```bash
python3 scripts/test_mesh_indices_host.py
```

It checks both widths, bounds, invalid layouts, unchanged versions, edits,
replacement handles, empty buffers and shared vertex-buffer ownership.
`IndexedMeshTest` in `test_gamesys.cpp` checks property assignment, null-adapter
draw objects, uploaded bytes, mutation, clearing and world-space rejection.
That integration fixture still needs a configured engine test build:

```bash
cmake --build engine/build/arm64-macos --target run_test_gamesys
```

The CI wasm runtime and Pi DrawElements path remain unverified until review
and the runtime build. The Stackrobats opt-in export reduces Bo from 29,628
vertices to 5,199 and Momo from 28,308 to 5,371. Triangle counts stay unchanged.
These are buffer counts, not measured shader invocations or Pi performance.

## WebGPU carts

The same build also produces a WebGPU cart (`wc_info_t.gpu_api = 2`) from
Defold's own WebGPU adapter, when it is pointed at the emdawnwebgpu release the
host's WebGPU glue was generated from. The cart carries the port's C++ half and
the host supplies the JavaScript half, so the two must be the same release
(currently `v20261002.154047`). A cart built with Emscripten's old
`-sUSE_WEBGPU` bindings is not compatible.

```bash
export DEFOLD_EMDAWNWEBGPU_PORT=/path/to/emdawnwebgpu_pkg/emdawnwebgpu.port.py
./scripts/build.py --platform=wasm-web --skip-tests build_engine -- \
    --skip-build-tests --with-wasmcart
# -> tmp/dynamo_home/bin/wasm-web/dmengine_wasmcart.wasm         (GL)
# -> tmp/dynamo_home/bin/wasm-web/dmengine_wasmcart_webgpu.wasm  (WebGPU)
```

Without `DEFOLD_EMDAWNWEBGPU_PORT` only the GL cart is built. The value is
cached by CMake, so a later build without the variable keeps using it.

Content needs WGSL shaders as well as GLSL ES. Add one flag to the bob command
above; the resulting archive serves both carts:

```bash
java -jar bob.jar --root <game> --platform wasm-web --archive \
    --use-uncompressed-lua-source --debug-output-wgsl true build
```

Pack it exactly like the GL cart, with `dmengine_wasmcart_webgpu.wasm` as the
`--wasm`.

How the WebGPU cart differs from Defold's browser WebGPU build:

- **No Asyncify, no adapter or device request.** The host owns the device and
  `emscripten_webgpu_get_device()` returns it synchronously, so the adapter
  skips `wgpuInstanceRequestAdapter`/`wgpuInstanceWaitAny`. There is no
  `WGPUAdapter`; feature queries go to the device.
- **No present.** The cart renders into the `#canvas` surface and the host
  presents after `wc_render`.
- **Validation errors are logged by the cart.** An imported device cannot take
  an uncaptured-error callback, so the adapter keeps a validation error scope
  open across each frame and pops it after the frame's submit. Errors arrive
  between frames and print as `WebGPU error (2): ...`.
- **Only the WebGPU adapter is linked.** The cart imports no GL functions.

The device is whatever the host created. Defold's sprite, label, GUI, particle
and physics content renders without validation errors on a
compatibility-feature-level device as well as a core one; texture arrays, cube
maps, compute and MSAA have not been exercised on either.

## How this repo is organised

The full Defold tree is **vendored** at upstream commit `540123b`, not forked.
The first commit is that tree verbatim, so `git diff <base>..HEAD` is an exact
account of what the port changes: 33 files, ~2800 lines.

Vendored wholesale because the build needs nearly all of it. `install_ext` does
**not** download - it extracts tarballs that must already be in `packages/` -
so `packages/` (368 MB), `external/` (155 MB) and `com.dynamo.cr/` (bob) all
have to be present. Dropping the editor alone saves 79 MB of 744 MB.

The port itself is eight new files plus edits threaded through existing ones:

| File | Replaces |
| --- | --- |
| `engine/platform/src/platform_window_wasmcart.cpp` | window, input, GL surface (was GLFW) |
| `engine/dlib/src/dlib/sys_wasmcart.cpp` | OS calls: paths, time, device info |
| `engine/engine/src/wasmcart/wasmcart_main.cpp` | `main()`; exports `wc_init` / `wc_render` |
| `engine/hid/src/native/hid_gamepad_driver_wasmcart.cpp` | gamepad driver |
| `engine/platform/src/platform_wasmcart_save.cpp` | `sys.save` into the ABI save block |
| `engine/sound/src/devices/device_wasmcart.cpp` | audio device |
| `engine/resource/src/mount/mount_wasmcart.cpp` | resources, mounted from the `.wasc` zip |

## ABI

wasmcart ABI v4. The cart exports `wc_get_info`, `wc_init` and `wc_render`, and
declares GLES3 as its GPU API. Pads, pointer, keyboard, audio ring, save block
and the host clock are all wired.

`wc_get_info` is idempotent: a host may call it again after `wc_init` to pick up
the resolved resolution, and some do.

## Docs

- [docs/README_BUILD.md](docs/README_BUILD.md) - building the engine
- [docs/README_SETUP.md](docs/README_SETUP.md) - toolchain and SDK setup
- [docs/README_EMSCRIPTEN.md](docs/README_EMSCRIPTEN.md) - the Emscripten
  toolchain this target is built with
- [docs/README_DEBUGGING.md](docs/README_DEBUGGING.md) - debugging the engine

`docs/upstream/` holds Defold's own project docs (contribution guide, release
process, iOS and Android notes, CI). They are kept for provenance and describe
upstream's project rather than this one, so most of them do not apply here.

## Related

The hosts a cart runs on are listed at the top. - [wasmcart-defold-examples](https://github.com/wasmcart/wasmcart-defold-examples)
  - carts built against this runtime, including five complete games

Other engines and languages that target the same format:

- [wasmcart-godot](https://github.com/wasmcart/wasmcart-godot) - Godot 4, the
  closest sibling to this repo and also a vendored engine
- [wasmcart-lua](https://github.com/wasmcart/wasmcart-lua) - Lua 5.4 with a
  LOVE-style API
- [wasmcart-pygame](https://github.com/wasmcart/wasmcart-pygame) - CPython 3.13
  and pygame
- [wasmcart-rust](https://github.com/wasmcart/wasmcart-rust),
  [wasmcart-zig](https://github.com/wasmcart/wasmcart-zig) - no_std and
  freestanding bindings
- [wasmcart-mruby](https://github.com/wasmcart/wasmcart-mruby) - Ruby
- [wasmcart-sdl2](https://github.com/wasmcart/wasmcart-sdl2) - porting toolkit
  for existing C/SDL2 games

Upstream: [defold/defold](https://github.com/defold/defold).

## Licence

Defold is under the [Defold License 1.0](https://defold.com/license/), which
permits modification ("You can modify the engine as much as you like") but whose
clause 4(a) forbids *selling* the work as a Game Engine Product, and the
definition explicitly covers the runtime. A free wasmcart-defold is fine; a paid
one, or a paid hosted service producing cart runtimes, is not.

`LICENSE.txt` and `NOTICE` are upstream's, kept verbatim, and the vendored
Defold sources remain under that licence.

The files this port adds carry the same Defold License header as the engine
code they sit beside, because they are backend implementations against Defold's
own internal interfaces and are not separable from it. There is no second
licence to reconcile.
