# Roadmap

Where Clockwork C stands and what comes next. Items are grouped by area and roughly ordered by priority within each; the *Known gaps* list is what the README already admits is missing or unverified. Tick a box when it lands and move finished sections to the bottom.

## Known gaps (fix first)

- [ ] **Restore the ECS core tests.** `tests/test_clockwork.c` and `examples/movement.c`, `save_load.c`, `visualizer.c` are referenced by the README and the build scripts but are not in the tree, so the core (entity lifetime, snapshots, corruption and bit-flip loads, the 12,000-operation reference model, clock limits) currently has no test in CI.
- [ ] **Windows verification.** `fix windows build` got the tree compiling with MSVC; still to do: run `build.ps1` and CTest end to end, `tools/build-game.ps1` module build and hot reload, export, and a D3D11 smoke run of the editor, launcher and player.
- [ ] **macOS verification.** `build-macos.sh` has never been executed: build, run the tests, and a Metal smoke run.
- [ ] **Compile the HLSL and MSL shaders.** The post-process and sprite uber shader variants mirror the GLSL ones but have not been compiled on D3D11 or Metal.
- [ ] **Custom material shaders on D3D11 and Metal.** `.cwshader` assets are GLSL only and fall back to the plain sprite effect elsewhere; needs a translation step (or an HLSL/MSL authoring path) so materials look the same on every backend.
- [ ] **orbit render settings.** `games/orbit` has no `render.cfg`; author one so both sample games exercise the settings path.
- [ ] **Update the README's validation section** once the above are done (it still says Windows and macOS were exercised on Linux only).

## Engine

- [ ] **Audio.** The asset registry already knows an `audio` type and `sound` fields pick audio assets, but nothing plays them. Add a `cw_audio` layer on `sokol_audio` (WAV/OGG decode, voices, buses, volume) and an engine `AudioSource` component like `Sprite`.
- [ ] **Gamepad input.** Only mouse and keyboard reach the host today; add controllers and an action map so games bind actions instead of keys.
- [ ] **Collision / physics helpers.** The scheduler deliberately steps no physics library; provide a small optional module (circle/AABB shapes, broadphase, overlap events) so every game does not rewrite combat's separation code.
- [ ] **Camera as a component.** Let a game drive the view through an engine `Camera` entity (follow, bounds, zoom, shake) instead of the host's `r2d_camera` only.
- [ ] **Tilemaps.** A tileset asset and a `Tilemap` component drawn through the sprite batch, with a paint tool in the editor.
- [ ] **Asset rename keeps references.** Renaming changes the id (hash of the name), so `@old` references stop resolving; rewrite references in scenes and prefabs on rename, or keep an alias table.
- [ ] **Prefab links.** Instances keep no link to their prefab; add linked instances with per-field overrides and propagation when the prefab is saved.
- [ ] **Networking.** The Odin original's `inbox`, `transport`, `predict`, `interp` and `pose` are out of scope so far; the ECS already has authority, replay and deterministic snapshots to build on.
- [ ] **Threading.** The API is single-threaded; consider parallel dispatch for systems that declare disjoint component access.

## Rendering

- [ ] **Particles and effects for modules.** `r2d_particles` and `r2d_effects` live in render2d but are not exposed through `cw_canvas`; add canvas ops (or engine components) so modules get the juice the old combat demo had.
- [ ] **Real fonts.** Text is the sokol debugtext pixel font; the registry has a `font` type but no loader. Add TTF rasterisation into an atlas for `UiText` and world labels.
- [ ] **Sprite animation from sheets.** `Sprite.frame` exists; add frame-sequence clips so sheets animate without a game system.

## Editor and tools

- [ ] **Animation curves.** The timeline has fixed interpolations; add tangent handles and a curve view.
- [ ] **Multi-scene editing.** Additive scenes or sub-scenes for large levels.
- [ ] **Play in a separate window** (or a second viewport) so the edited scene and the running game can be seen side by side.
- [ ] **Profiler panel.** Per-system timings and the render stats (`r2d_post_get_stats`, `r2d_lighting_get_stats`) in the editor.
- [ ] **Launcher templates.** More than one starter (empty, top-down, platformer) to pick from when creating a project.

## Quality

- [ ] **CI.** Run `build.sh --sanitize` and CTest on Linux, and the CMake build on Windows and macOS, on every push.
- [ ] **Scripted UI sessions in CI.** The xdotool sessions that validated the editor and launcher are not checked in; turn them into a reproducible script under Xvfb.
- [ ] **Fuzz the text formats.** Scenes, prefabs, materials, clips and the project file with the same rigour as the snapshot bit-flip test.
