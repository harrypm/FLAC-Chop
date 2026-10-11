#!/usr/bin/env bash
# CLI <-> GUI 1:1 parity gate (the MISRC automated-GUI-testing model).
#
# Drives the SAME binary through both surfaces on the SAME input:
#   - CLI:  --probe --json, then a whole-file chop.
#   - GUI:  a --gui launch with the FLAC_CHOP_GUI_TEST hooks — the probe +
#     plan state dump, then FLAC_CHOP_GUI_TEST_RUN drives the cut through
#     the exact Process path a user clicks (the real event loop, thread
#     watchers, settings), and the run report is dumped before self-exit.
#
# Asserts 1:1 parity: probe fields equal, both runs verify PASS with the
# same sample count, identical STREAMINFO facts, and byte-identical decoded
# audio (the FLAC containers differ legitimately — the GUI embeds the
# pre-conversion metadata editor's extra rows — so the DECODED samples are
# the parity-relevant bytes).
#
# The fixture is a 20 MSPS .lds on purpose: the GPU path engages only for
# exactly-40-MSPS captures, so both sides deterministically take the SoX
# path on every machine (CI runners have no GPU; real machines may).
#
# Usage: scripts/gui_cli_parity.sh <flac-chop-binary> [workdir]
# Needs: sox + soxi + sha256sum + python3 on PATH (or FLAC_CHOP_SOX set).
# Headless-safe: set QT_QPA_PLATFORM=offscreen when there is no display.
set -euo pipefail

BIN="$(readlink -f "${1:?usage: gui_cli_parity.sh <flac-chop-binary> [workdir]}")"
WORK="$(readlink -f "${2:-$(mktemp -d)}")"
mkdir -p "$WORK"
# sox for the script's own decode/inspect steps (`sox --i` is the soxi
# info command — no separate soxi binary needed): PATH first, else the
# FLAC_CHOP_SOX override. The flac-chop binary resolves ITS sox through
# the core's own detector either way.
SOX_BIN="sox"
if ! command -v sox >/dev/null 2>&1 && [ -n "${FLAC_CHOP_SOX:-}" ]; then
  SOX_BIN="$FLAC_CHOP_SOX"
fi
IN="$WORK/parity_20msps.lds"
# The GUI's auto-derived output name: the input stem + "-cut.flac"
# (the .lds EXTENSION is dropped — completeBaseName semantics).
GUI_OUT="${IN%.lds}-cut.flac"

# --- fixture: 20,000 packing groups (80,000 samples) of on-format 10-bit
# values — the reference ld-lds-converter 4-samples-in-5-bytes layout.
python3 - "$IN" <<'EOF'
import sys
data = bytearray()
for i in range(20000):
    v = [(i * 37 + j * 111) % 1024 for j in range(4)]
    w0, w1, w2, w3 = v
    b0 = (w0 >> 2) & 0xFF
    b1 = ((w0 & 0x03) << 6) | ((w1 >> 4) & 0x3F)
    b2 = ((w1 & 0x0F) << 4) | ((w2 >> 6) & 0x0F)
    b3 = ((w2 & 0x3F) << 2) | ((w3 >> 8) & 0x03)
    b4 = w3 & 0xFF
    data += bytes([b0, b1, b2, b3, b4])
open(sys.argv[1], "wb").write(data)
print(f"fixture: {len(data)} bytes = 80000 samples")
EOF

# --- CLI side: probe (JSON) + whole-file chop (0.004 s = 80000 samples at
# 20 MSPS, clamped to the file end).
"$BIN" --probe "$IN" --json > "$WORK/cli_probe.json"
"$BIN" "$IN" "$WORK/cli_out.flac" 0 0.004 > "$WORK/cli_chop.txt" 2>&1
echo "--- CLI chop output ---"; cat "$WORK/cli_chop.txt"
test -f "$WORK/cli_out.flac"

# --- GUI side: launch with the load-test hooks. The GUI writes its output
# beside the input (auto-derived <stem>-cut.flac) and self-exits after the
# run report.
rm -f "$WORK/gui_report.json" "$GUI_OUT" 2>/dev/null || true
FLAC_CHOP_GUI_TEST=1 \
FLAC_CHOP_GUI_TEST_RUN=1 \
FLAC_CHOP_GUI_TEST_OUT="$WORK/gui_report.json" \
FLAC_CHOP_GUI_TEST_QUIT=1 \
  "$BIN" --gui "$IN" > "$WORK/gui_launch.txt" 2>&1
test -f "$WORK/gui_report.json"
test -f "$GUI_OUT"

# --- assertions (probe parity, run parity, decoded-audio parity)
python3 - "$WORK" <<'EOF'
import json, sys
w = sys.argv[1]
cli = json.load(open(f"{w}/cli_probe.json"))
gui = json.load(open(f"{w}/gui_report.json"))

assert cli["ok"] is True, "CLI probe failed"
gp = gui["probe"]
assert gp["ok"] is True, "GUI probe failed"
assert cli["format_code"] == gp["format"], \
    f"format mismatch: CLI {cli['format_code']} vs GUI {gp['format']}"
assert int(cli["real_rate_hz"]) == int(gp["real_rate_hz"]), "real rate mismatch"
assert int(cli["total_samples"]) == int(gp["total_samples"]), "total mismatch"
assert cli["total_samples"] == 80000, "fixture total wrong"
print("PASS: probe parity (format, real rate, total samples)")

run = gui["run"]
assert run["ok"] is True, f"GUI run failed: {run}"
# run["verify"] is the GUI's BRIEF verdict (no sample count); the full
# verify line with the count lives in the notes.
assert run["verify"].startswith("verify: PASS"), f"GUI verify verdict: {run['verify']}"
assert "verify: PASS (80000 samples" in run["notes"], "GUI notes missing the full verify verdict"
assert "sha256(output):" in run["notes"], "GUI notes missing the output checksum"
print("PASS: GUI run report (ok, verify PASS 80000, sha256 note)")

cli_chop = open(f"{w}/cli_chop.txt").read()
assert "verify: PASS (80000 samples" in cli_chop, "CLI chop verify verdict missing"
assert "sha256(output):" in cli_chop, "CLI chop checksum note missing"
print("PASS: CLI run report (ok, verify PASS 80000, sha256 note)")
EOF

# STREAMINFO facts identical (rate/bits/channels/samples).
for f in "$WORK/cli_out.flac" "$GUI_OUT"; do
  "$SOX_BIN" --i "$f" > "$WORK/soxi_$(basename "$f").txt"
done
GUI_SOXI="$WORK/soxi_$(basename "$GUI_OUT").txt"
for key in "Channels" "Sample Rate" "Precision" "Duration"; do
  a="$(grep -E "^$key" "$WORK/soxi_cli_out.flac.txt" | head -1)"
  b="$(grep -E "^$key" "$GUI_SOXI" | head -1)"
  test "$a" = "$b" || { echo "FAIL: $key differs: [$a] vs [$b]"; exit 1; }
done
echo "PASS: STREAMINFO parity (channels, rate, precision, duration)"

# Decoded audio byte-identical (containers may differ: the GUI embeds the
# pre-conversion editor's extra metadata rows; the DATA is the parity bit).
"$SOX_BIN" "$WORK/cli_out.flac" -t raw -e signed-integer -b 16 "$WORK/cli.raw"
"$SOX_BIN" "$GUI_OUT" -t raw -e signed-integer -b 16 "$WORK/gui.raw"
CLI_SHA="$(sha256sum < "$WORK/cli.raw" | cut -d' ' -f1)"
GUI_SHA="$(sha256sum < "$WORK/gui.raw" | cut -d' ' -f1)"
test "$CLI_SHA" = "$GUI_SHA" || {
  echo "FAIL: decoded audio differs: cli=$CLI_SHA gui=$GUI_SHA"; exit 1; }
echo "PASS: decoded audio identical ($CLI_SHA)"

echo ""
echo "CLI <-> GUI 1:1 parity: ALL CHECKS PASSED ($IN)"
