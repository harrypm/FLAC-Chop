# HARD DEV NOTE — dark-theme input-box text contrast (every Qt GUI)

Status: HARD RULE for every Qt6 GUI in the family (FLAC-Chop, tbc-tools, tape-decode-rust GUI, vhs-decode, future apps). A dark palette is NOT done until the input-box roles below are set and measured. This bug has now been hit and fixed repeatedly (tbc-tools hit it across several tools; FLAC-Chop hit it again on 2026-10-03) — stop re-hitting it.

Canonical shared implementation: `tbc-tools/src/library/tbc/uistyle.h` (`enforceInputWidgetContrast`, `stockDarkPalette`, `ThemedApplication`). This note is the distilled rule; FLAC-Chop carries a faithful port in `gui/theme.h` (see also DEV_NOTE_theme_switch_one_click.md — the full one-click switching flow; the old `gui/main.cpp applyDarkFusion()` is gone).

## The bug (root cause)
A hand-built dark palette made from a **default-constructed QPalette** keeps the LIGHT-theme defaults for the roles nobody remembers to set. The visible ones are inside QLineEdit "file selection" boxes (read-only path fields, output-dir fields, time boxes):

- `QPalette::PlaceholderText` resolves from the default (light) palette — measured on FLAC-Chop's Linux Mint/Qt 6.11 it came out **#000000** on the `Base` #191919 background: literally invisible placeholder hints ("(follows input: …)", "(each input file's own folder)").
- `QPalette::Disabled, QPalette::Text` / `Disabled, PlaceholderText` likewise: disabled boxes (fields frozen while a job runs) render black-on-black.

Setting `Text`/`Base` white-on-dark is NOT enough — Qt does not derive PlaceholderText from your Text; it keeps the default palette's value.

## The fix (minimum, every dark palette)
```cpp
d.setColor(QPalette::PlaceholderText, QColor(0xD0, 0xD4, 0xD9));
d.setColor(QPalette::Disabled, QPalette::Text, QColor(0xAA, 0xAF, 0xB5));
d.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(0x8D, 0x93, 0x99));
app.setPalette(d);
// Guard stylesheet (tbc-tools enforceInputWidgetContrast): pin input-widget
// colors so a platform theme override cannot re-darken them.
app.setStyleSheet(QStringLiteral(
    "QLineEdit, QTextEdit, QPlainTextEdit {"
    "  color: palette(text);"
    "  selection-color: palette(highlighted-text);"
    "  selection-background-color: palette(highlight);"
    "}"));
```
Light theme equivalents (0x5F6368 placeholder, 0x6B7280 / 0x9AA0A6 disabled) are in uistyle.h.

## The full tbc-tools solution (adopt what applies)
- `enforceInputWidgetContrast()`: measure, don't assume — `|lightnessF(color) − lightnessF(Base)|` must be ≥ **0.45** for Text, ≥ **0.2** for PlaceholderText (also guards Highlight vs HighlightedText). Fix only when below the threshold.
- `ThemedApplication` (uistyle.h): re-asserts the stock palette on `ApplicationPaletteChange` (the macOS scheduled Dark-Mode switchover silently re-reads the system palette and overwrites yours mid-run). Deferred with `QTimer::singleShot(0,…)` — a synchronous setPalette inside `event()` recurses and crashes. FLAC-Chop adopted the full `ThemedApplication` port (gui/theme.h) on 2026-10-05 — see DEV_NOTE_theme_switch_one_click.md (the runtime switch needs it everywhere, not just macOS).
- `QGuiApplication::setDesktopSettingsAware(false)` before QApplication + `styleHints()->setColorScheme(...)` (Qt ≥ 6.8) as defense in depth — now part of FLAC-Chop's startup (gui/theme.h prepareStockThemeEnvironment).
- Read-only path boxes: use QPalette::Text (not a hard-coded grey) so the theme owns the color.

## How to verify (hard data, not "looks fine")
Measure the resolved roles against Base lightness (the A/B harness used on 2026-10-03, /tmp/fc_batch_harness/contrast_check.cpp):

OLD (bug): PlaceholderText #000000 vs Base #191919 → dist 0.10 — FAIL (threshold 0.2)
           Disabled/PlaceholderText #000000 → dist 0.10 — FAIL
NEW (fix): PlaceholderText #d0d4d9 → dist 0.74 OK; Disabled/Text #aaafb5 → 0.59 OK; Disabled/PlaceholderText #8d9399 → 0.48 OK

Then eyeball the real GUI: the Output Directory placeholder on the Chop tab and the Batch Task output-dir placeholder must be clearly readable grey-on-dark, including while a job runs (disabled state). Final proof is the user's screen, not the numbers.

## Checklist for any new Qt GUI or theme change
1. Set Base, Text, AND PlaceholderText, AND the Disabled variants of Text/PlaceholderText.
2. Measure the lightness distances (0.45 / 0.2 thresholds).
3. Add the guard stylesheet for QLineEdit/QTextEdit/QPlainTextEdit.
4. macOS ships: use the ThemedApplication re-assert + setDesktopSettingsAware(false) + setColorScheme.
5. Verify on the real screen (placeholders visible, enabled AND disabled).
