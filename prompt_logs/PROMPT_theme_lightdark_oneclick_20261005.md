# PROMPT: Lite/Dark theme (one-click) + dark default (2026-10-05)

## Inputs (user prompts, in order)
1. "Also add a theme tab at the top for Lite/Dark modes like tbc-tools has."
2. "Not a sub window" / "It should be just Dark / Light" / "Like File/Hlep" — a Theme MENU in the top bar (a peer of File/Help), just Dark / Light, not a tab.
3. "Light does nothing effectivly, I said 'like tbc-tools' analyse has a fully working and debugged implimeantion to ref ffs"
4. "tbc-analyse" / "ld naming has been stripped away now" — the reference is the current tbc-tools (the local checkout is stale: still ld-analyse; pulled via the GitHub API instead because the local tbc-tools git store is corrupted).
5. "Nope it has the dubble click bug which was docmumented and fixed..."
6. "still requires dubble click..."
7. "make a hard dev note for this bug fix"
8. "Also quick change on file Open FLAC to Open Data as it supports more then FLAC only input"
9. "Make darkmode deafult"

## The reference studied (remote tbc-tools, via the GitHub API)
- `src/library/tbc/uistyle.h` — the canonical implementation: `stockDarkPalette()`, `stockLightPalette()`, `prepareStockThemeEnvironment()` (setDesktopSettingsAware(false) BEFORE construction), `enforceInputWidgetContrast()` (the measured adaptive pass), `ThemedApplication` (property-before-setPalette, setColorScheme (Qt>=6.8), the deferred second pass, the ApplicationPaletteChange re-assert with the m_reasserting guard), `applyStockDarkThemeToApp()/applyStockLightThemeToApp()`.
- `src/tbc-analyse/main.cpp` — `isDarkModeEnabled()` (the OS detection) + `applyDarkTheme()`.
- The documented double-click fixes live IN the code comments (property-first ordering + deferred second pass).

## Bugs found by real-data testing (all fixed)
1. **"Light does nothing"** (my first naive build): Light used the style's `standardPalette()` — on a dark-mode OS (this machine: AppsUseLightTheme=0) Qt's Fusion follows the platform colour scheme, so the "standard" palette IS DARK; plus desktop-settings-aware Qt re-reads the OS palette over manual ones. Fix: the full port above (explicit stockLightPalette, setDesktopSettingsAware(false), setColorScheme).
2. **The double-click bug** — after the full port it STILL "required a double click". Traced with an env-gated stderr trace (FLAC_CHOP_DEBUG_THEME=1): every user click landed in the PALETTE (dark→#353535, light→#EFEFEF) yet they clicked twice — and on-screen PIXEL measurement proved the screen never repainted: `QWidget::update()` after `QApplication::setPalette()` does NOT repaint the window on Windows/Qt 6.11 — the pixels always showed the PREVIOUS theme (palette dark / pixels light (243,242,242); palette light / pixels dark (66,66,66)). The user's second click was only earning the repaint. Fix: `forceCompleteRepaint()` — clear + re-apply the app stylesheet (a full style re-polish) + synchronously `repaint()` every top-level and child widget, in the immediate and deferred passes.
   Also observed: each apply triggers a long ApplicationPaletteChange cascade (~3000 events at startup) — the platform fighting the manual palette (#323232 = the OS dark palette) and the re-assert answering; it settles and is guarded, but it is why the tbc-tools event() re-assert + m_reasserting guard exist.
3. Stale local tbc-tools checkout (ld-analyse naming) + a corrupted local git object store — read the reference from the GitHub API instead.

## Verification (commands + hard results)
- Pixel measurement (CopyFromScreen over the window rect, sampled grid average) — BEFORE the repaint fix: STARTUP palette=light pixels=(243,242,242); after ONE synthetic Alt+T,L (palette=light): pixels=(66,66,66) — one theme BEHIND. AFTER the fix: STARTUP (light choice saved then) (243,242,242) → ONE Alt+T,D → (66,66,66) → ONE Alt+T,L → (243,242,242) — ONE CLICK each, both directions.
- Trace (FLAC_CHOP_DEBUG_THEME): 3 applies (startup + Dark + Light), each landing; the user's earlier real-click sequence confirmed the palette switched first-time every time.

## Changes made
- `gui/theme.h` (new, header-only): the faithful tbc-tools port + the `forceCompleteRepaint()` Windows fix + the env-gated debug trace.
- `gui/theme.cpp` (new): only `systemPrefersDark()` (kept for parity; the app default is DARK).
- `gui/main.cpp`: `prepareStockThemeEnvironment()` + `ThemeUi::ThemedApplication` construction; startup: saved choice else DARK (the default).
- `gui/mainwindow.cpp`: Theme menu (File / Theme / Help; exclusive checkable Dark / Light; reflects the active choice; applies + persists QSettings "theme/mode"); applyThemeChoice goes through the ThemedApplication ToApp helpers. File menu: "Open FLAC..." → "Open Data...".
- `gui/mainwindow.h`: applyThemeChoice decl.
- `gui/CMakeLists.txt`: theme.cpp/theme.h in GUI_SOURCES.
- `docs/DEV_NOTE_theme_switch_one_click.md` (new hard dev note; the contrast note's stale FLAC-Chop references updated).
- `readme.md`: the theme feature bullet.

## Still awaiting user confirmation (on their screen)
- The open instance: opens DARK (default); Theme → Light switches in ONE click; Theme → Dark back in one; the choice is remembered.
- The earlier question is still open: which dialog shows "Vista/7 elements" (the file picker / the folder picker) — my window-class enumeration shows the file dialogs ARE the native Windows ones (#32770) in this build.
