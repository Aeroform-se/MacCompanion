#!/usr/bin/env bash
# Injects a ReadMe text file into a Classic Mac HFS .dsk image:
# - Converts UTF-8 typography (em-dash, en-dash, bullets, accents) to Mac OS Roman
# - Converts LF / CRLF line endings to Classic Mac CR (\r) for SimpleText / TeachText
# - Copies into the root of the HFS volume and sets Type='TEXT', Creator='ttxt'
set -euo pipefail

DSK_PATH="$1"
README_SRC="$2"
HFS_FILENAME="${3:-$(basename "$README_SRC")}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HFS_BIN_DIR="$ROOT_DIR/toolchain/Retro68-build/toolchain/bin"

if [ ! -f "$DSK_PATH" ] || [ ! -f "$README_SRC" ]; then
    echo "Usage: $0 <path-to.dsk> <path-to-ReadMe.txt> [HFS-filename]"
    exit 1
fi

TMP_MAC_TXT="$(mktemp /tmp/mac_readme.XXXXXX)"
trap 'rm -f "$TMP_MAC_TXT"' EXIT

python3 - "$README_SRC" "$TMP_MAC_TXT" <<'PYEOF'
import sys

src_path, dst_path = sys.argv[1], sys.argv[2]
with open(src_path, "r", encoding="utf-8") as f:
    text = f.read()

# Normalize CRLF / LF to Classic Mac OS CR (\r) for TeachText / SimpleText
text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r")

# Encode into Classic Mac OS Roman (with ASCII fallback for any unmapped symbol)
mac_bytes = text.encode("mac_roman", errors="replace")
with open(dst_path, "wb") as f:
    f.write(mac_bytes)
PYEOF

"$HFS_BIN_DIR/hmount" "$DSK_PATH" >/dev/null
"$HFS_BIN_DIR/hcopy" -r "$TMP_MAC_TXT" ":${HFS_FILENAME}"
"$HFS_BIN_DIR/hattrib" -t TEXT -c ttxt ":${HFS_FILENAME}"
"$HFS_BIN_DIR/humount" >/dev/null

# Also stamp macOS / Basilisk II FinderInfo ('DDim' / 'ddsk') on the .dsk file itself
python3 - "$DSK_PATH" <<'PYEOF'
import subprocess, sys
dsk_path = sys.argv[1]
finfo = b"DDimddsk\x01\x00" + (b"\x00" * 22)
hex_finfo = finfo.hex()
for attr in ("com.apple.FinderInfo", "org.BasiliskII.FinderInfo"):
    subprocess.run(["xattr", "-wx", attr, hex_finfo, dsk_path], check=False)
PYEOF

echo "    + Bundled '${HFS_FILENAME}' (Mac Roman CR, TEXT/ttxt) into $(basename "$DSK_PATH")"
