# Bundled Sokol

The desktop snapshot example uses these unmodified Sokol headers from commit
`bbac2ef1bf9d0240225e7507071cc6c08482f965`:

- `sokol_app.h`, `sokol_gfx.h`, `sokol_glue.h`, `sokol_log.h`
- `util/sokol_gl.h`, `util/sokol_debugtext.h`

Upstream: [Sokol](https://github.com/floooh/sokol).
The original archive URL and SHA-256 are recorded in `../../tools/sokol.lock`
for provenance. Builds use the files in this directory and never download them.
Required upstream copyright, attribution and license notices are preserved in
the headers and `LICENSE`.
