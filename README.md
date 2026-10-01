# wasmcart-defold

**Write games in Defold. Ship them as wasmcart cartridges.**

The [Defold](https://defold.com) engine compiled to standalone WebAssembly as a
[wasmcart](https://github.com/wasmcart/wasmcart) runtime. One
`dmengine_wasmcart.wasm` is built once; any Defold game becomes a cart by
packing its bob output beside it. The same `.wasc` runs on every wasmcart host.

This is Defold without a browser. Defold's own HTML5 export needs a DOM, a
WebGL canvas and JS glue; this is standalone WASM with none of that.

Upstream Defold is **vendored, not forked** (see
[How this repo is organised](#how-this-repo-is-organised)). Upstream's own
README is kept as `README_DEFOLD.md`.

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
`README_BUILD.md`, upstream's own instructions).

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
[wasmcart-defold-examples](https://github.com/wasmcart/wasmcart-defold-examples).

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

wasmcart ABI v3. The cart exports `wc_get_info`, `wc_init` and `wc_render`, and
declares GLES3 as its GPU API. Pads, pointer, keyboard, audio ring, save block
and the host clock are all wired.

`wc_get_info` is idempotent: a host may call it again after `wc_init` to pick up
the resolved resolution, and some do.

## Licence

Defold is under the [Defold License 1.0](https://defold.com/license/), which
permits modification ("You can modify the engine as much as you like") but whose
clause 4(a) forbids *selling* the work as a Game Engine Product, and the
definition explicitly covers the runtime. A free wasmcart-defold is fine; a paid
one, or a paid hosted service producing cart runtimes, is not.

Vendored Defold sources remain under that licence. See `LICENSE.txt` and
`NOTICE`.
