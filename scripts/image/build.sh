#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -m) != aarch64 || $(id -u) != 0 || ! -e /.dockerenv ]]; then
  echo 'Run this image builder as root inside a privileged native ARM64 Debian trixie Docker container.' >&2
  exit 1
fi
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
git config --global --add safe.directory "$repo"
dist=$(realpath "${1:?release dist directory required}")
output=$(realpath -m "${2:?output directory required}")
server_url=${3:?HTTPS enrollment server URL required}
business_config=$(realpath "${4:-$repo/config/config.example.json}")
work=$(realpath -m "${5:-$repo/build/factory-image}")
mkdir -p "$output" "$work/rpi"
shopt -s nullglob
archives=("$dist"/cns-rpi-*-trixie-arm64.tar.gz)
[[ ${#archives[@]} == 1 ]] || { echo 'Exactly one ARM64 release archive is required.' >&2; exit 1; }
archive=${archives[0]}
(cd "$dist" && sha256sum -c "$(basename "$archive").sha256")
digest=$(cut -d ' ' -f1 "$archive.sha256")
source_commit=$(git -C "$repo" rev-parse HEAD)
image_gen_commit=$(tr -d '\r\n' < "$repo/image/rpi-image-gen.rev")
python3 "$repo/scripts/image/seed.py" config --archive "$archive" \
  --sha256 "$digest" --source-commit "$source_commit" \
  --business-config "$business_config" --server-url "$server_url" \
  --output "$work/runtime.yaml"
tool="$work/rpi-image-gen"
git clone https://github.com/raspberrypi/rpi-image-gen.git "$tool"
git -C "$tool" checkout --detach "$image_gen_commit"
[[ $(git -C "$tool" rev-parse HEAD) == "$image_gen_commit" ]]
"$tool/install_deps.sh"
chmod +x "$repo/image/hooks/"*
export SOURCE_DATE_EPOCH=$(git -C "$repo" show -s --format=%ct HEAD)
"$tool/rpi-image-gen" build -S "$repo/image" -c "$work/runtime.yaml" -B "$work/rpi"
raw="$work/rpi/image-cns-rpi-trixie-arm64-pi5/cns-rpi-trixie-arm64-pi5.img"
mountpoint="$work/readback"
mkdir -p "$mountpoint"
offset=$(sfdisk --json "$raw" | python3 -c 'import json,sys; table=json.load(sys.stdin)["partitiontable"]; print(table["partitions"][1]["start"] * table["sectorsize"])')
cleanup() {
  mountpoint -q "$mountpoint" && umount "$mountpoint"
}
trap cleanup EXIT
mount -o "ro,loop,offset=$offset" "$raw" "$mountpoint"
python3 "$repo/scripts/image/verify.py" --root "$mountpoint" \
  --source-commit "$source_commit" --output "$output/readback.json"
chroot "$mountpoint" dpkg-query -W '-f=${binary:Package}\t${Version}\t${Architecture}\n' > "$output/packages.tsv"
for name in cns_rpi cns_control_test; do
  chroot "$mountpoint" ldd "/opt/cns-rpi/current/bin/$name" > "$output/$name.ldd.txt"
  if grep -q 'not found' "$output/$name.ldd.txt"; then
    echo 'Image readback found a missing runtime library.' >&2
    exit 1
  fi
done
umount "$mountpoint"
trap - EXIT
name="cns-rpi-trixie-arm64-pi5-${source_commit:0:12}.img.xz"
xz -T2 -c "$raw" > "$output/$name"
(cd "$output" && sha256sum "$name" > "$name.sha256")
python3 - "$output" "$archive" "$digest" "$image_gen_commit" "$name" <<'PY'
import hashlib, json, pathlib, sys
output, archive, release_sha, image_gen_sha, name = sys.argv[1:]
output = pathlib.Path(output)
record = json.loads((output / 'readback.json').read_text())
record.update(schema_version=1, release_archive=pathlib.Path(archive).name,
              release_sha256=release_sha, image_gen_commit=image_gen_sha,
              image=name, image_sha256=hashlib.file_digest((output / name).open('rb'), 'sha256').hexdigest(),
              packages='packages.tsv', verification='offline-image-readback',
              hardware_boot_verified=False, published=False)
(output / 'build-record.json').write_text(json.dumps(record, indent=2) + '\n')
PY
