# Clockwork C

A standalone C11 port of the core ECS in Exmortalis' `engine/ECS/clockwork`. It has no dependency on Exmortalis, Odin, the renderer, physics, or third-party libraries. All source, tests, and build files are comment-free.

## Build

On Windows with Visual Studio C++ Build Tools:

```powershell
.\build.ps1
.\build.ps1 -Release
.\build.ps1 -Sanitize
```

Each invocation builds `clockwork.lib`, runs the regression tests, builds and runs the movement example, and builds the interactive save/load tool and runs its headless self-check. Outputs go into a configuration-specific directory under `build`.

`-Compiler clang` selects an installed Visual Studio Clang toolchain. `-Sanitize` requires the AddressSanitizer toolchain component. `-NoGui` skips the save/load tool.

With CMake and a C11 toolchain:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake --config Debug
ctest --test-dir build/cmake -C Debug --output-on-failure
```

On Linux, `./build.sh [--compiler gcc|clang] [--release] [--sanitize] [--no-gui]` does the same as `build.ps1`, including the headless visualizer check. For GCC or Clang on Linux/macOS, a direct build of the library and tests needs only:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/*.c tests/test_clockwork.c -o clockwork_tests
./clockwork_tests
```

The save/load tool (`examples/save_load.c`) is the one component with a dependency: it uses a vendored 2D subset of [raylib](https://www.raylib.com) and raygui (zlib license) in `third_party/raylib`, built from source by all three build paths. On Linux it needs the X11 development headers (`libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev libxext-dev` on Debian/Ubuntu); CMake skips the tool with a warning when they are missing, `build.sh --no-gui`, `build.ps1 -NoGui` and `-DCW_BUILD_GUI=OFF` skip it explicitly. The library itself never depends on raylib.

The platform layer uses `VirtualAlloc` on Windows and `mmap`/`mprotect` on POSIX. The virtual address reservation is intended for 64-bit processes. Windows builds are verified locally; POSIX requires validation on a POSIX host.

## Included scope

| Odin source | C implementation |
| --- | --- |
| `clockwork.odin`, `entity.odin` | IDs, entity flags, 192-bit component signatures |
| `world.odin` | Creation, restoration, rebuilding, deferred destruction, reset, bulk reservation, optional ID recycling |
| `pool.odin` | Sparse pools, presence bitsets, stable addresses during ordinary growth, initialization and cleanup callbacks |
| `query.odin` | Bitset intersection, ascending entity iteration, collection |
| Core of `registry.odin` | Component ID blocks, names, discovery, transient flags, explicit codec callbacks |
| `sim.odin` | Ordered systems, lifecycle phases, pause behavior, authority filtering, shared Update replay |
| `tick.odin` | Fixed-step clock and backlog catch-up policy |

The engine component definitions in `ECS/comps`, reflection-driven inspector and console, and networking helpers (`inbox`, `transport`, `predict`, `interp`, `pose`) are outside this ECS-only project. C has no Odin runtime type reflection, so automatic field inspection and automatic JSON fallback are not included. Serialization formats are supplied through component codec callbacks.

## Use

Include `clockwork.h` and link the static library, or compile the sources in `src` into your program. CMake consumers can use `add_subdirectory` and link `clockwork::clockwork`. `examples/movement.c` demonstrates the complete world/component/system lifecycle. `examples/save_load.c` is an interactive tool for the save/load API: buttons populate and tick a world, mark and sweep entities, save it (visible-state serialization or a full snapshot), destroy the world, load the file into a fresh world registered with the other schema order, verify it against what was saved, peek at the file, create entities afterwards, and provoke failures (a codec-less component, an unwritable path, a corrupted file) to show the error reporting and that a failed save leaves the file untouched. `clockwork_save_load --headless [path]` runs the same actions as a script and prints `OK: save/load verified`; the build scripts use that as a regression check.

1. Initialize a `cw_world` with `cw_world_init` and a `cw_sim` with `cw_sim_init`.
2. Reserve component IDs with `cw_comp_block`. Register ordinary types with `CW_REGISTER`, or call `cw_comp_register` to supply a name, size, alignment, and cleanup callback.
3. Create entities with `cw_world_create_entity`. Set data with `cw_comp_set`, or add a component using `cw_comp_add` and its optional initializer.
4. Build a signature with `cw_mask`, then iterate using `cw_query_iter` and `cw_query_next`. Access registered slots through `CW_GET`.
5. Register systems with `cw_sim_register`. Set `authority` explicitly to `CW_HOST_ONLY` or `CW_SHARED`; a zero-initialized C descriptor uses `CW_SHARED`.
6. Call `cw_sim_tick`, or dispatch phases yourself. Call Init/Start/Stop/Shutdown explicitly when appropriate for your application.
7. To persist a world, give each saved component a codec (or allow raw copies), call `cw_snapshot_save` into a `cw_writer` (full state) or `cw_world_serialize` into an owned `cw_buffer` (visible state only), and later register the same component names in a fresh world and call `cw_snapshot_load` or `cw_world_deserialize`. `cw_snapshot_peek` inspects an image without a schema.
8. Destroy the simulation and world when finished.

## Behavioral contracts

Entity IDs are signed 64-bit integers with `-1` invalid. IDs are not recycled by default. Destroying an entity marks it immediately invisible to queries while keeping its data readable by ID through EntityDestroyed dispatch. The sweep then releases its components. Enabling `recycle_ids` allows stale IDs to alias new entities, as in Odin. Reset restarts IDs at zero and invalidates all old references.

`cw_world_restore_entity` returns true when it creates the requested ID. For an already active entity it cancels any pending destruction, preserves its data, and returns false. `cw_world_rebuild_entity` clears owned components, cancels pending destruction, and records a creation event while retaining the ID.

Created and pending-destruction views are borrowed arrays. The created list records creation events, so an entity created and destroyed in one tick may already be invalid when EntityCreated runs. EntityCreated runs after the sweep, then clears the creation list. Do not keep view pointers across world mutations. Creating entities during EntityCreated follows the original behavior: they join that same list, which is cleared after dispatch.

Every component pool reserves virtual address space for 16,777,216 slots by default and commits pages as capacity grows. This does not immediately consume that amount of physical memory. Define `CW_POOL_RESERVE_ENTITIES` when compiling the library to change the initial reservation. Component pointers remain stable while growth fits the reservation. Exceeding it relocates that pool and invalidates its old pointers. Removal, destruction, rebuilding, reset, and world destruction invalidate the affected logical component references. Entity-array pointers may change at every world capacity increase.

`CW_GET` checks registration, element size, and entity validity, but does not require component presence. Check `cw_comp_has` before reading a component that may be absent. Raw slot writes do not set presence; use `cw_comp_set` or `cw_comp_add`. Size checking cannot distinguish two C types with identical sizes. Invalid IDs and component IDs fail safely. Names are copied at registration.

`cw_comp_set` copies bytes and marks presence, without invoking initialization or cleanup. When replacing resource-owning values, release the previous resources yourself or remove the component first. `cw_comp_add` invokes initialization only when absent. Cleanup runs once on removal, rebuilding, sweep, reset, or destruction; a custom cleanup callback replaces default zeroing and must leave the slot reusable. Callbacks may release resources and edit their slot, but must not mutate the world or registry. Tags can be registered with size zero and alignment one. Component alignment must be a power of two no larger than the OS page size, and element size must be a multiple of alignment.

Queries require every component in the mask. An empty mask visits all visible entities. Marking/removing components during iteration is supported, including removal from another entity whose bit was already cached. Additions or restorations during an existing query are not guaranteed to appear; restart the query to observe them. Resetting or destroying a world during iteration is unsupported. Collect functions return the full result count and write at most the supplied output capacity; pass NULL/zero to measure.

System dispatch order is ascending `order`, then registration sequence. Named replacement retains its sequence and re-enables the system; unnamed systems are appended. Registration/removal during dispatch returns false. Enable/disable is permitted during dispatch and affects subsequent systems. Do not destroy the simulation while it is dispatching. Scheduler introspection pointers are borrowed and may change after sorting or registration. The scheduler does not step any physics library.

Pause gates EarlyUpdate, PhysicsUpdate, Update, LateUpdate, and EndFrame unless a system opts into `run_when_paused`. Lifecycle and user phases ignore pause. A tick still processes entity lifecycle and increments `world.tick` while paused. Client role skips host-only systems. Replay runs enabled shared Update systems regardless of pause.

Codec callbacks define the serialized format. Marshal takes a caller-owned buffer and reports required/written bytes through `size`; NULL/zero measures. Unmarshal writes a registered live slot and marks presence only on success. The callback must preserve the prior value on failure and handle any resource replacement. Missing callbacks return false; bytes are never silently treated as JSON. Transient is metadata for the snapshot save policy and does not disable explicit marshal calls.

`cw_writer` and `cw_reader` are the bounds-checked byte primitives behind snapshots, and codecs are encouraged to build on them: every multi-byte integer is little-endian, floats are IEEE-754 bit patterns, and signed values are two's complement. A growable writer never fails (allocation failure aborts); a fixed writer sets a sticky overflow flag, keeps counting the bytes required and never writes past its capacity; a NULL fixed writer only measures, which is exactly the marshal contract. A reader fails every access past its end, stores zero, leaves its position unchanged and sets a sticky error so a run of reads can be checked once. Size arithmetic is overflow-checked rather than wrapping, and `cw_reader_sub` bounds a length-prefixed span to its own reader.

Snapshots (`cw_snapshot_save`, `cw_snapshot_load`, `cw_snapshot_peek`) serialise a world into a versioned little-endian image whose layout is documented in `clockwork.h`: a magic/version/flags/size header, then tagged, length-prefixed sections for world state, the component schema, entity records and the recycled id list. Unknown sections are skipped so the format can grow; an unknown version or flag is rejected. The image holds every active entity, including those marked for destruction, its components, the tick, `next_id`, `recycle_ids`, the recycled list in order and `last_created`, so entity ids survive exactly and a loaded world allocates the same ids the saved world would have. Cached queries, observers, command buffers, the created list and the user pointer are not part of the image; the registry is not either, which is why the loading world must have registered the schema first. Components are matched by registered name, never by id, so registration order may differ between the saving and loading worlds.

A component is encoded by its marshal callback, or, without one and only with `allow_raw` in the save config, as the raw bytes of its slot; raw payloads are host layout, and the loader requires `allow_raw`, an identical element size and the same host byte order (recorded in the header flags). Without a codec and without `allow_raw` the save fails with `CW_SNAPSHOT_ERROR_NO_CODEC` naming the component. Transient components are skipped unless `include_transient` is set, and only components that some entity carries appear in the schema. A save appends to the caller's writer (growable, fixed, or measuring) and on failure truncates it back to its initial size; a fixed writer that is too small reports the bytes required in `error.offset`.

`cw_world_serialize` produces the same image format in a buffer the caller owns (`cw_buffer`, released with `cw_buffer_free`), but captures only the observable state: visible entities (active and not marked for destruction) with their ids, their non-transient components by registered name, the tick and `next_id`. Pending destructions, transient components, the recycled id list and `last_created` are omitted, and the image carries an empty recycled list; because `next_id` is kept, the omitted ids are never handed out again after a load. A snapshot of a world with nothing pending, no recycled ids and `last_created` cleared is byte-identical to its serialization. On failure nothing is allocated and the buffer is empty. The result loads and peeks like any snapshot.

`cw_world_deserialize` is the strict counterpart: it rebuilds a serialized image in a fresh world whose schema the caller has registered, resolving component types by name (registration order and ids may differ from the saving world), and restores entity ids, components, the tick and `next_id` exactly, so entities created afterwards get the ids the original world would have issued. Unknown component names and unsupported format versions are always rejected, and `check_user_version` additionally rejects a different application version. Failure never yields a half-built world: structure, limits and schema are validated before any entity exists, and if a codec rejects a payload later the entities built so far are released through the normal reset path (cleanup callbacks run, observers see the removals) and the world is fresh again with its prior settings; the call returns false in every such case.

The file helpers (`cw_snapshot_save_file`/`cw_snapshot_load_file`, `cw_world_serialize_file`/`cw_world_deserialize_file`, and the plain `cw_file_write`/`cw_file_read`) wrap these memory APIs with whole-file I/O and explicit ownership: paths, worlds and configs are borrowed for the call, and the buffer `cw_file_read` fills belongs to the caller until `cw_buffer_free`. A save encodes the complete image in memory first, writes it to a sibling temporary (`path.tmp`), flushes it to disk, and only then renames it over `path`, so neither an encoding failure nor an I/O failure can damage a valid file already there, and a crash mid-write leaves at most the temporary behind. A load reads the whole file (bounded by `max_size`, default `CW_FILE_DEFAULT_MAX_SIZE`) and then applies the memory API's guarantees. `cw_file_error` carries a file status, the `errno` captured at the failing call, and for `CW_FILE_ERROR_SNAPSHOT` the full `cw_snapshot_error`. The `examples/save_load.c` tool exercises all of this interactively and in its headless self-check.

A load requires a fresh world (a new or reset one: no entities, `next_id` zero, no recycled ids) and a load config; zero fields take `CW_SNAPSHOT_DEFAULT_*`, `cw_snapshot_load_defaults` spells them out, and `SIZE_MAX`/`INT64_MAX` mean unlimited. Every count, id and payload length is validated against `max_entities`, `max_entity_id` and `max_component_bytes`, and every schema entry against the registry, before the world is touched, so untrusted input cannot commit memory or reach a callback; the corruption checks also cover section structure, ascending unique ids, ids below `next_id`, duplicate components, unknown flags, and recycled ids that are live or repeated. Unknown component names fail with `CW_SNAPSHOT_ERROR_UNKNOWN_COMPONENT` unless `ignore_unknown_components` is set, in which case those payloads are skipped. Only a codec can still reject a payload during the apply phase; the world is then reset, so after any failure it is fresh again and `error` reports the status, byte offset, entity and component involved. Loaded entities are reported in `cw_world_created` like restored ones, so EntityCreated systems can rebuild transient state, and pending destructions are re-marked. Trailing bytes after an image are an error; `cw_snapshot_peek` reports an image's size and header fields for callers embedding snapshots in larger streams.

The API is single-threaded. Worlds and simulations must be initialized once and destroyed before reuse; copying initialized objects by value is unsupported. Allocation failure and size overflow abort, matching the source implementation's fatal allocation policy. Invalid negative or non-finite frame times are ignored. No external allocator, thread scheduler, or dependency graph is introduced.

## Validation

The test executable covers entity lifetime, all signature-word boundaries, invalid arguments, callback counts, restoration/rebuild, recycling/reset, an 18,000-entity reservation and growth regression, alignment, tags, codecs, byte writer/reader bounds and counter-overflow behaviour at every width, snapshot round-trips across differently numbered schemas with byte-exact re-saves and id-allocator determinism, world serialization decoded record by record from the documented layout (framing, schema entries, entity records, payload bytes, exclusions), deserialization of empty and sparse worlds with tags and multi-component entities into a differently ordered schema followed by further entity creation, rejection of unknown names and versions, and release of a partially built world on codec failure, file helpers (round trips, size limits, I/O and argument errors, failed saves leaving the previous file byte-identical with no temporary left behind), every snapshot error status, load limits, header/section/record corruption, truncation at every prefix length, 3,000 random bit-flip loads, system ordering/replacement, authority/pause/replay, lifecycle order, dispatch mutation guards, and clock limits. A deterministic 12,000-operation reference-model test checks random restore/set/remove/destroy/sweep/rebuild sequences against a separate state model. Tests remain enabled in release builds.

The original port was validated on Windows with MSVC debug and optimized release builds, MSVC AddressSanitizer, and CMake/CTest; the installed Clang AddressSanitizer also completes the tests, but emits a runtime interceptor warning. The byte buffer and snapshot work was validated on Linux with GCC debug, release and AddressSanitizer/UndefinedBehaviorSanitizer builds via `build.sh`, and with CMake/CTest; all 31 test groups, the movement example, the save/load tool's headless check and the headless visualizer pass there; the tool's window was exercised under Xvfb with scripted clicks and under AddressSanitizer. The vendored raylib has not been built with MSVC here. It has not yet been run under MSVC.

Small deliberate corrections from Odin are the monotonic scheduler registration sequence after removals, rechecking cached query candidates after component removal, and safe rejection of invalid component operations and invalid frame deltas. Other core lifecycle behavior is retained.
