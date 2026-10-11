# PROMPT LOG — Wiki: Replace ld-compress info with FLAC-Chop CLI & GUI notes
Date: 2026-10-09
Repo context: FLAC-Chop (C:\Users\Harry\flac-chop), target file outside repo:
`F:\000 RF Decode Wiki GitHubs\0000 Github Atom Editing\Offical VHS Decode Github Wiki\RF-Compression-&-Decompression-Guide.md`

## User input (verbatim intent)
> F:\000` RF` Decode` Wiki` GitHubs\0000` Github` Atom` Editing\Offical` VHS` Decode` Github` Wiki\RF-Compression-`&-Decompression-Guide.md
> Replace all ld-compress info with FLAC Chop CLI and GUI notes

(Note: the backtick characters in the user's path were escape artifacts; real folder names use spaces, e.g. `F:\000 RF Decode Wiki GitHubs\...`.)

## User shell commands (pre-prompt, ran themselves)
- `cd '..'`
- `cd 'flac-chop'`

## Research performed (hard data, no assumptions)
1. Read target wiki file (373 lines original) — located ld-compress references:
   - DdD section line 140 (`.ldf` output description) + TIP line 154-155 (`ld-compress` drag-and-drop)
   - `## Linux Scripts` + `## .lds` 10-bit Packed to FLAC (lines 158-177, `ld-compress.bat` / `ld-compress-nvidia-gpu`)
   - Linux Down-Sampling Scripts (lines 192-278, six `ld-lds-converter | sox` pipe scripts)
   - `# Windows Scripts` (lines 322-331: lds-compress.bat / lds-compress-nvidia-gpu.bat / lds-unpack.bat / s16-to-flac.bat)
   - Windows Users section line 317 (link to CXADC-Compression-Scripts/Windows bundle)
   - Flow diagram line 45 (`ld-lds-converter` chain)
2. Read `C:\Users\Harry\flac-chop\readme.md` — CLI usage, GUI tabs, supported inputs, MISRC output naming.
3. Read `gui\main.cpp` (lines 11-429) — verified exact CLI forms, `--gui`/`--probe`/`--version`, out-as-dir naming, seconds/samples clamping to probed end.
4. Read `core\src\chop.rs` (lines 116-314) — verified wiki-aligned sinc presets: 10→0-3050, 16→0-7650, 20→0-9650, 24→0-9400, 28.6→0-9400.
5. Read `core\src\ffi.rs` — `fc_generate_output_path`: empty stem → `<inputstem>-cut.flac`.
6. Read `F:\...\RF-Compression-Notes-editing.md` — scratch pad of ld-lds-converter pipes, no FLAC-Chop draft notes present.

## Commands run by agent (PowerShell)
- `Get-ChildItem 'F:\' -Directory` — locate real folder names (spaces, not backticks)
- `Get-ChildItem 'F:\000 RF Decode Wiki GitHubs' ...` and subdir listings — find the wiki folder
- `git -C <wiki> rev-parse --is-inside-work-tree` → true (repo)
- `Copy-Item ... RF-Compression-&-Decompression-Guide.pre-flacchop.bak.md -Force` — backup created
- Line-splice (PowerShell Get-Content/rebuild, CRLF preserved, no BOM): replaced lines 190-278 with s16 NOTE + `## Down-Sampling With FLAC-Chop (Linux / Windows / macOS)` section (presets table, three CLI one-liners, .u8 note, Batch Task pointer). 373 → 323 lines.
- `edit_files` — 7 diffs: flow diagram, DdD line 140, DdD TIP, `## Linux Scripts`+`## .lds` → `## FLAC-Chop CLI (all platforms)` + `## FLAC-Chop GUI`, Windows Users FLAC-Chop pointer, `# Windows Scripts` FLAC-Chop release/installer notes, Bit Crushing `--bits 6`/`--bits 12` TIP.
- `Select-String` verification — remaining `ld-lds`/`ld-compress` mentions are intentional context only (lines 35 ld-lds-reader decode-side; 165/226/307 "replaces the old ..." sentences).
- `git --no-pager diff --stat` — guide: 160 lines changed (76 insertions/97 deletions across the 3-file working tree; the Hardware-Installation-Guide.md and Submitting-&-Downloading-Sample-Data.md changes pre-date this session and were not touched).

## Files edited
- `F:\000 RF Decode Wiki GitHubs\0000 Github Atom Editing\Offical VHS Decode Github Wiki\RF-Compression-&-Decompression-Guide.md` (in place, CRLF, UTF-8 no BOM)
- Backup: `RF-Compression-&-Decompression-Guide.pre-flacchop.bak.md` (same folder)

## Not done / left as-is (deliberate)
- `ld-lds-reader` mention (line 35) — decode-side reading, not compression.
- SoX Commands section, s16 ffmpeg command, Bit Crushing ffmpeg method, compression tables — not ld-compress; kept, with FLAC-Chop TIPs added where directly relevant.
- Not committed; git working tree left for user review.
