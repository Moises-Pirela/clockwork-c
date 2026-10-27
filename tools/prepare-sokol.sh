#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "$0")/.." && pwd)
source_path=$project_root/third_party/sokol
required_files=(sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h util/sokol_gl.h util/sokol_debugtext.h LICENSE)
for file_name in "${required_files[@]}"; do
    if [ ! -f "$source_path/$file_name" ]; then
        echo "Bundled Sokol file missing: $file_name. Restore third_party/sokol from the repository." >&2
        exit 1
    fi
done
printf '%s\n' "$source_path"
