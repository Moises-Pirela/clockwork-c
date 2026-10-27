# Clockwork C

A standalone C11 port of the core ECS in Exmortalis' `engine/ECS/clockwork`. It has no dependency on Exmortalis, Odin, the renderer, physics, or third-party libraries. All source, tests, and build files are comment-free.

## Build

On Windows with Visual Studio C++ Build Tools:

```powershell
.\build.ps1
.\build.ps1 -Release
.\build.ps1 -Sanitize
```

Each invocation builds `clockwork.lib`, runs the regression tests, and builds and runs the movement example. Outputs go into a configuration-specific directory under `build`.

`-Compiler clang` selects an installed Visual Studio Clang toolchain. `-Sanitize` requires the AddressSanitizer toolchain component.

With CMake and a C11 toolchain:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake --config Debug
ctest --test-dir build/cmake -C Debug --output-on-failure
```

For GCC or Clang on Linux/macOS, a direct build needs only:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude src/clockwork.c src/cw_sim.c tests/test_clockwork.c -o clockwork_tests
./clockwork_tests
```

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

Include `clockwork.h` and link the static library, or compile `src/clockwork.c` and `src/cw_sim.c` into your program. CMake consumers can use `add_subdirectory` and link `clockwork::clockwork`. `examples/movement.c` demonstrates the complete world/component/system lifecycle.

1. Initialize a `cw_world` with `cw_world_init` and a `cw_sim` with `cw_sim_init`.
2. Reserve component IDs with `cw_comp_block`. Register ordinary types with `CW_REGISTER`, or call `cw_comp_register` to supply a name, size, alignment, and cleanup callback.
3. Create entities with `cw_world_create_entity`. Set data with `cw_comp_set`, or add a component using `cw_comp_add` and its optional initializer.
4. Build a signature with `cw_mask`, then iterate using `cw_query_iter` and `cw_query_next`. Access registered slots through `CW_GET`.
5. Register systems with `cw_sim_register`. Set `authority` explicitly to `CW_HOST_ONLY` or `CW_SHARED`; a zero-initialized C descriptor uses `CW_SHARED`.
6. Call `cw_sim_tick`, or dispatch phases yourself. Call Init/Start/Stop/Shutdown explicitly when appropriate for your application.
7. Destroy the simulation and world when finished.

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

Codec callbacks define the serialized format. Marshal takes a caller-owned buffer and reports required/written bytes through `size`; NULL/zero measures. Unmarshal writes a registered live slot and marks presence only on success. The callback must preserve the prior value on failure and handle any resource replacement. Missing callbacks return false; bytes are never silently treated as JSON. Transient is metadata for a caller's save policy and does not disable explicit marshal calls.

The API is single-threaded. Worlds and simulations must be initialized once and destroyed before reuse; copying initialized objects by value is unsupported. Allocation failure and size overflow abort, matching the source implementation's fatal allocation policy. Invalid negative or non-finite frame times are ignored. No external allocator, thread scheduler, or dependency graph is introduced.

## Validation

The test executable covers entity lifetime, all signature-word boundaries, invalid arguments, callback counts, restoration/rebuild, recycling/reset, an 18,000-entity reservation and growth regression, alignment, tags, codecs, system ordering/replacement, authority/pause/replay, lifecycle order, dispatch mutation guards, and clock limits. A deterministic 12,000-operation reference-model test checks random restore/set/remove/destroy/sweep/rebuild sequences against a separate state model. Tests remain enabled in release builds.

Validated on Windows with MSVC debug and optimized release builds, MSVC AddressSanitizer, and CMake/CTest. All 14 test groups and the movement example pass. The installed Clang AddressSanitizer also completes the tests, but emits a runtime interceptor warning; the clean sanitizer result is from MSVC. No POSIX execution has been performed.

Small deliberate corrections from Odin are the monotonic scheduler registration sequence after removals, rechecking cached query candidates after component removal, and safe rejection of invalid component operations and invalid frame deltas. Other core lifecycle behavior is retained.
