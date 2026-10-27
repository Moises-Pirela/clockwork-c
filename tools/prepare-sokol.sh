#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "$0")/.." && pwd)
dependency_root=${1:-$project_root/build/dependencies}
{
    IFS= read -r dependency_url
    IFS= read -r expected_hash
} < "$project_root/tools/sokol.lock"
dependency_url=${dependency_url%$'\r'}
expected_hash=${expected_hash%$'\r'}
revision=${dependency_url##*/}
source_path=$dependency_root/sokol-$revision
required_files=(sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h util/sokol_gl.h util/sokol_debugtext.h LICENSE)
complete=1
for file_name in "${required_files[@]}"; do
    [ -f "$source_path/$file_name" ] || complete=0
done
if [ "$complete" = 1 ] && [ -f "$source_path/.clockwork-ready" ] && [ "$(cat "$source_path/.clockwork-ready")" = "$expected_hash" ]; then
    printf '%s\n' "$source_path"
    exit 0
fi
verify_hash() {
    local actual_hash
    if command -v sha256sum >/dev/null 2>&1; then
        actual_hash=$(sha256sum "$1")
    else
        actual_hash=$(shasum -a 256 "$1")
    fi
    [ "${actual_hash%% *}" = "$expected_hash" ]
}
mkdir -p "$dependency_root"
dependency_root=$(cd "$dependency_root" && pwd)
source_path=$dependency_root/sokol-$revision
archive_path=$dependency_root/sokol-$revision.zip
if [ ! -f "$archive_path" ]; then
    curl --fail --location --silent --show-error "$dependency_url" -o "$archive_path.download"
    if ! verify_hash "$archive_path.download"; then
        rm -f "$archive_path.download"
        echo 'Sokol dependency checksum mismatch.' >&2
        exit 1
    fi
    mv "$archive_path.download" "$archive_path"
fi
if ! verify_hash "$archive_path"; then
    echo 'Sokol dependency checksum mismatch; remove the cached archive and retry.' >&2
    exit 1
fi
tar -xf "$archive_path" -C "$dependency_root"
for file_name in "${required_files[@]}"; do
    [ -f "$source_path/$file_name" ] || { echo "Sokol dependency missing $file_name" >&2; exit 1; }
done
printf '%s' "$expected_hash" > "$source_path/.clockwork-ready"
printf '%s\n' "$source_path"
