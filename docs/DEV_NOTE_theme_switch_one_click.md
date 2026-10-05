# HARD DEV NOTE — runtime Lite/Dark theme switching (one click, both palettes)

Status: HARD RULE for every Qt6 GUI in the family (FLAC-Chop, tbc-tools, tape-decode-rust GUI, vhs-decode, future apps). A runtime Dark/Light switch is NOT done unless it goes through the stock-palette `ThemedApplication` flow below. Never hand-roll a `setPalette()` theme switch. Hit and fixed in tbc-tools (`uistyle.h`, documented in-code); FLAC-Chop re-hit BOTH bugs on 2026-10-05 with a naive implementation (Light did nothing on a dark-mode Windows machine; the menu needed a second click) — stop re-hitting it.

Canonical shared implementation: `tbc-tools/src/library/tbc/uistyle.h` (GPL-3.0-or-later, (c) Simon Inns) — `ThemedApplication`, `stockDarkPalette()`, `stockLightPalette()`, `prepareStockThemeEnvironment()`, `enforceInputWidgetContrast()`. FLAC-Chop carries a faithful port in `gui/theme.h` (header-only) + `gui/theme.cpp` (`systemPrefersDark`), constructed as `ThemeUi::ThemedApplication` in `gui/main.cpp`, driven by the Theme menu in `gui/mainwindow.cpp`.

## Bug 1 — "Light does nothing" on a dark-mode OS

A naive Light implementation (`app.setPalette(app.style()->standardPalette())` or "use the platform default palette") is a no-op on a machine whose OS is in dark mode, because:

1. Qt ≥ 6.5's Fusion style follows the **platform color scheme** — on a dark-mode OS the "standard"/default palette IS DARK. Light built on it re-applies dark.
2. Qt is **desktop-settings-aware by default**: on application palette changes it re-reads the system palette and silently overwrites a manually-applied palette.

Fix (all three, from uistyle.h):
- An **explicit `stockLightPalette()`** — hand-built light values with the same role coverage as the dark palette (never `standardPalette()`).
- `QGuiApplication::setDesktopSettingsAware(false)` — via `prepareStockThemeEnvironment()` — **before** QApplication construction.
- (Qt ≥ 6.8) `styleHints()->setColorScheme(Qt::ColorScheme::Dark|Light)` — asks the platform to override the system color scheme and ignore its changes.

## Bug 2 — the double-click bug (a theme click looks like it needs a second click)

Three separate documented causes:

1. **The `isDarkTheme` property must be set BEFORE `setPalette()`.** `setPalette()` synchronously propagates PaletteChange to every widget; any widget that reads the app `isDarkTheme` property during that propagation (custom-painted widgets: plots, scopes, anything theme-aware in `paintEvent`) still sees the PREVIOUS value and repaints with the stale theme — the switch appears to need a second click.
2. **A direct (non-system) `setPalette()` can leave custom-painted widgets half-resolved** until a second event (QGraphicsView/scene-based widgets, widgets caching the theme). Fix: after the initial apply, re-render every top-level + child widget, then a **deferred second pass** (`QTimer::singleShot(0, ...)`) that re-applies the stock palette + the contrast guard and re-renders again.
3. **(Windows, verified by on-screen pixel measurement on Qt 6.11) `QWidget::update()` after `QApplication::setPalette()` does NOT repaint the window** — the palette object switches but the on-screen pixels keep the PREVIOUS theme (measured: palette dark / pixels light, then palette light / pixels dark — always one theme behind), so every menu click appears to "need a second click" purely to earn a repaint. Fix: `forceCompleteRepaint()` — synchronously `repaint()` every top-level + child widget AND clear + re-apply the application stylesheet (a stylesheet change forces a full style re-polish of every widget). After this fix, one synthetic menu trigger (Alt+T, D / L) measured dark (66,66,66) and light (243,242,242) — ONE click each.

## Bug 3 — the OS switchover overwrite (macOS scheduled Dark Mode change)

Qt re-reads the system palette on `ApplicationPaletteChange` and overwrites the manually-applied stock theme mid-run. Fix: override `event()` and re-assert the chosen stock palette — **deferred** (`QTimer::singleShot(0, ...)`) and guarded (`m_reasserting`), because a synchronous `setPalette()` inside `event()` recurses (`setPalette -> ApplicationPaletteChange -> setPalette …`) and crashes.

## The full flow (do not skip steps)

```
before QApplication:   ThemeUi::prepareStockThemeEnvironment();     // setDesktopSettingsAware(false) + style-override normalize
construct:             ThemeUi::ThemedApplication app(argc, argv);
startup / menu click:  app.applyStockDarkTheme() / applyStockLightTheme()
                       (or ThemeUi::applyStockDarkThemeToApp() from a widget)
inside applyStockTheme(dark):
  1. setProperty("isDarkTheme", dark)   // BEFORE setPalette (bug 2.1)
  2. applyFusionStyleIfAvailable()
  3. setPalette(dark ? stockDarkPalette() : stockLightPalette())  // explicit palettes (bug 1)
  4. Qt>=6.8: styleHints()->setColorScheme(...)                   // platform override (bug 1)
  5. enforceInputWidgetContrast()       // the measured contrast pass (see DEV_NOTE_dark_theme_input_contrast.md)
  6. forceCompleteRepaint()             // stylesheet re-polish + synchronous repaint() (bug 2.3, Windows)
  7. deferred second pass: palette + contrast + forceCompleteRepaint  // bug 2.2
event() on ApplicationPaletteChange:
  deferred re-assert of the chosen stock palette (m_reasserting guard) // bug 3
```

## How to verify (hard data, not "looks fine")

The one-click test, on a machine whose OS is in DARK mode (the failure case):

1. Launch: the saved choice (or the OS default) is applied — no half-themed widgets.
2. Theme → Light: the whole window switches to light in **one** click — no second click, no stale-dark patches, placeholders readable (the contrast pass).
3. Theme → Dark: back to dark in one click.
4. Restart: the last choice is remembered (QSettings "theme/mode"); deleting the setting falls back to the OS theme.

Final proof is the user's screen, both themes, enabled AND disabled controls.

## Checklist for any new Qt GUI or theme change

1. Never `standardPalette()`/platform-default for Light — explicit `stockLightPalette()`.
2. `prepareStockThemeEnvironment()` before construction; `ThemedApplication` as the app class.
3. Property → style → palette → colorScheme → contrast → repaint → deferred second pass, in that order.
4. The `event()` re-assert with the `m_reasserting` guard.
5. The one-click test on a dark-mode OS, both directions, plus a restart.
