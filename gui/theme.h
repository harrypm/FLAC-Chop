#ifndef FLACCHOP_THEME_H
#define FLACCHOP_THEME_H

// Lite/Dark theming — a faithful port of tbc-tools' theme model
// (src/library/tbc/uistyle.h, GPL-3.0-or-later, (c) Simon Inns), which is the
// fully working + debugged implementation used by tbc-analyse (ex
// ld-analyse) and the other tbc-tools GUIs:
//  - stockDarkPalette()/stockLightPalette(): the authoritative palettes.
//  - prepareStockThemeEnvironment(): call BEFORE QApplication construction
//    (stops Qt re-reading the desktop palette, which would otherwise
//    overwrite a manually-applied theme — the reason a naive "Light" does
//    nothing on a dark-mode OS).
//  - ThemedApplication: the QApplication subclass that applies the stock
//    themes in ONE click (the documented double-click fixes: the
//    isDarkTheme property is set BEFORE setPalette() so custom-painted
//    widgets don't repaint the stale theme, and a deferred second pass
//    completes custom-painted widgets that a direct setPalette() leaves
//    half-resolved until a second event) and re-asserts the choice whenever
//    the platform signals an application palette change (e.g. the macOS
//    Dark Mode switchover).
//  - Theme menu (mainwindow, a peer of File/Help): Dark / Light, applied
//    immediately + persisted (QSettings "theme/mode"); the default follows
//    the OS theme.

#include <QApplication>
#include <QColor>
#include <cstdio>
#include <QGuiApplication>
#include <QMargins>
#include <QPalette>
#include <QRect>
#include <QScreen>
#include <QStyleFactory>
#include <QStyle>
#include <QStyleHints>
#include <QtGlobal>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace ThemeUi {

inline qreal paletteContrastDistance(const QColor &first, const QColor &second)
{
    return qAbs(first.lightnessF() - second.lightnessF());
}

inline QColor preferredInputTextColor(bool darkBase)
{
    return darkBase ? QColor(0xF5, 0xF7, 0xFA) : QColor(0x16, 0x18, 0x1C);
}

inline QColor preferredPlaceholderColor(bool darkBase)
{
    return darkBase ? QColor(0xD0, 0xD4, 0xD9) : QColor(0x5F, 0x63, 0x68);
}

inline QColor preferredHighlightedTextColor(bool darkHighlight)
{
    return darkHighlight ? QColor(0xF5, 0xF7, 0xFA) : QColor(0x20, 0x21, 0x24);
}

inline void normalizeUnsupportedStyleOverrideToFusion()
{
    const QByteArray styleOverride = qgetenv("QT_STYLE_OVERRIDE").trimmed();
    if (styleOverride.isEmpty()) {
        return;
    }

    const QString requestedStyle = QString::fromLocal8Bit(styleOverride);
    const QStringList availableStyles = QStyleFactory::keys();
    const bool styleSupported = availableStyles.contains(requestedStyle, Qt::CaseInsensitive);
    if (!styleSupported && availableStyles.contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
        qputenv("QT_STYLE_OVERRIDE", QByteArrayLiteral("Fusion"));
    }
}

inline void applyFusionStyleIfAvailable(QApplication &application)
{
    if (!QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
        return;
    }
    const QString currentStyleName =
        application.style() ? application.style()->objectName() : QString();
    if (currentStyleName.compare(QStringLiteral("Fusion"), Qt::CaseInsensitive) == 0) {
        return;
    }
    application.setStyle(QStringLiteral("Fusion"));
}

// Stock dark Fusion palette (neutral grey/black, legacy blue Highlight).
inline QPalette stockDarkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(53, 53, 53));
    palette.setColor(QPalette::WindowText, QColor(255, 255, 255));
    palette.setColor(QPalette::Base, QColor(25, 25, 25));
    palette.setColor(QPalette::AlternateBase, QColor(64, 64, 64));
    palette.setColor(QPalette::ToolTipBase, QColor(53, 53, 53));
    palette.setColor(QPalette::ToolTipText, QColor(255, 255, 255));
    palette.setColor(QPalette::Text, QColor(255, 255, 255));
    palette.setColor(QPalette::Button, QColor(53, 53, 53));
    palette.setColor(QPalette::ButtonText, QColor(255, 255, 255));
    palette.setColor(QPalette::BrightText, QColor(0xFF, 0x55, 0x55));
    palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(160, 160, 160));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(160, 160, 160));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(160, 160, 160));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(255, 255, 255));
    return palette;
}

// Stock light Fusion palette — manual opt-in. Same role coverage as the dark
// palette so the contrast guard resolves consistently regardless of which
// stock theme is active.
inline QPalette stockLightPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(239, 239, 239));
    palette.setColor(QPalette::WindowText, QColor(0, 0, 0));
    palette.setColor(QPalette::Base, QColor(255, 255, 255));
    palette.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
    palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 255));
    palette.setColor(QPalette::ToolTipText, QColor(0, 0, 0));
    palette.setColor(QPalette::Text, QColor(0, 0, 0));
    palette.setColor(QPalette::Button, QColor(239, 239, 239));
    palette.setColor(QPalette::ButtonText, QColor(0, 0, 0));
    palette.setColor(QPalette::BrightText, QColor(0xFF, 0x55, 0x55));
    palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(200, 200, 200));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0, 0, 0));
    return palette;
}

// Must be called before QApplication construction. Disables Qt's tracking of
// desktop palette/font changes (defense-in-depth against the OS appearance
// switchover re-overwriting a manually-applied stock theme; the authoritative
// re-assert lives in ThemedApplication::event()).
inline void prepareStockThemeEnvironment()
{
    QGuiApplication::setDesktopSettingsAware(false);
    normalizeUnsupportedStyleOverrideToFusion();
}

inline void enforceInputWidgetContrast(QApplication &application)
{
    QPalette palette = application.palette();
    const QColor inputBackground = palette.color(QPalette::Base);
    const bool darkBase = inputBackground.lightnessF() < 0.5;

    QColor inputText = palette.color(QPalette::Text);
    if (paletteContrastDistance(inputBackground, inputText) < 0.45) {
        inputText = preferredInputTextColor(darkBase);
    }

    QColor inputPlaceholder = palette.color(QPalette::PlaceholderText);
    if (paletteContrastDistance(inputBackground, inputPlaceholder) < 0.2) {
        inputPlaceholder = preferredPlaceholderColor(darkBase);
    }

    const QColor inputHighlight = palette.color(QPalette::Highlight);
    const bool darkHighlight = inputHighlight.lightnessF() < 0.5;
    QColor inputHighlightedText = palette.color(QPalette::HighlightedText);
    if (paletteContrastDistance(inputHighlight, inputHighlightedText) < 0.45) {
        inputHighlightedText = preferredHighlightedTextColor(darkHighlight);
    }

    palette.setColor(QPalette::Text, inputText);
    palette.setColor(QPalette::PlaceholderText, inputPlaceholder);
    palette.setColor(QPalette::HighlightedText, inputHighlightedText);
    palette.setColor(QPalette::Disabled, QPalette::Text, darkBase ? QColor(0xAA, 0xAF, 0xB5) : QColor(0x6B, 0x72, 0x80));
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, darkBase ? QColor(0x8D, 0x93, 0x99) : QColor(0x9A, 0xA0, 0xA6));
    application.setPalette(palette);

    const QString guardMarker = QStringLiteral("FC_INPUT_CONTRAST_GUARD");
    if (application.styleSheet().contains(guardMarker)) {
        return;
    }

    QString styleSheet = application.styleSheet();
    if (!styleSheet.isEmpty()) {
        styleSheet.append(QLatin1Char('\n'));
    }

    styleSheet.append(QStringLiteral(
        "/* %1 */"
        "QLineEdit,"
        "QTextEdit,"
        "QPlainTextEdit {"
        "  color: palette(text);"
        "  selection-color: palette(highlighted-text);"
        "  selection-background-color: palette(highlight);"
        "}").arg(guardMarker));

    application.setStyleSheet(styleSheet);
}

// ThemedApplication pins the stock Fusion palette (dark by default, light via
// applyStockLightTheme()) and re-asserts it whenever the platform signals an
// application palette change — e.g. a scheduled Dark Mode switchover.
// Without this, Qt re-reads the system palette on the switchover and
// overwrites the manually-applied palette, breaking theming mid-run.
//
// The re-assert is deferred with QTimer::singleShot(0, ...) so it runs *after*
// QApplication's default ApplicationPaletteChange propagation to top-level
// windows/widgets; re-applying the palette synchronously inside event() can
// recurse (setPalette -> ApplicationPaletteChange -> setPalette ...) and
// crash. On Qt >= 6.8, setColorScheme() asks the platform to override the
// system color scheme and ignore its changes.
class ThemedApplication : public QApplication
{
public:
    explicit ThemedApplication(int &argc, char **argv)
        : QApplication(argc, argv)
    {
    }

    // Debug trace (FLAC_CHOP_DEBUG_THEME=1): every theme step to stderr, so a
    // real click sequence can be observed from outside the process.
    static void trace(const char *what, bool dark, bool paletteToo)
    {
        if (!qEnvironmentVariableIsSet("FLAC_CHOP_DEBUG_THEME"))
            return;
        const QColor w = palette().color(QPalette::Window);
        std::fprintf(stderr, "[theme] %s dark=%d Window=#%02X%02X%02X%s\n",
                     what, int(dark), w.red(), w.green(), w.blue(),
                     paletteToo ? "" : " (pre-setPalette)");
        std::fflush(stderr);
    }

    void applyStockDarkTheme() { applyStockTheme(true); }
    void applyStockLightTheme() { applyStockTheme(false); }

protected:
    bool event(QEvent *event) override
    {
        if (event && event->type() == QEvent::ApplicationPaletteChange) {
            trace("event: ApplicationPaletteChange", m_reassertDark, true);
            const bool result = QApplication::event(event);
            if (!m_reasserting) {
                m_reasserting = true;
                QTimer::singleShot(0, this, [this]() {
                    QApplication::setPalette(m_reassertDark ? stockDarkPalette()
                                                           : stockLightPalette());
                    enforceInputWidgetContrast(*this);
                    trace("event: deferred re-assert applied", m_reassertDark, true);
                    m_reasserting = false;
                });
            }
            return result;
        }
        return QApplication::event(event);
    }

private:
    void applyStockTheme(bool dark)
    {
        trace("applyStockTheme: start", dark, false);
        m_reassertDark = dark;
        // Set the isDarkTheme property BEFORE setPalette(): setPalette()
        // synchronously propagates PaletteChange to every widget, and
        // custom-painted widgets read this property during that propagation.
        // If it's still the previous value, they repaint with the stale
        // theme and the switch appears to need a second click.
        setProperty("isDarkTheme", dark);
        applyFusionStyleIfAvailable(*this);
        setPalette(dark ? stockDarkPalette() : stockLightPalette());
        trace("applyStockTheme: palette set", dark, true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        trace("applyStockTheme: setColorScheme done", dark, true);
#endif
        enforceInputWidgetContrast(*this);
        trace("applyStockTheme: contrast pass done", dark, true);

        forceCompleteRepaint();

        // Deferred second pass: a direct (non-system) setPalette() can leave
        // some custom-painted widgets partially resolved until a second
        // event. The palette-change path already re-asserts via a deferred
        // timer and that completes in one pass; mirror that here so a menu
        // click switches fully without needing a second click. Harmless at
        // startup (same palette). m_reasserting prevents the event()
        // override from stacking another deferred re-assert on top.
        m_reasserting = true;
        QTimer::singleShot(0, this, [this]() {
            QApplication::setPalette(m_reassertDark ? stockDarkPalette()
                                                   : stockLightPalette());
            enforceInputWidgetContrast(*this);
            forceCompleteRepaint();
            trace("applyStockTheme: deferred second pass done", m_reassertDark, true);
            m_reasserting = false;
        });
    }

    // Force every widget to re-render NOW. On Windows/Qt 6.11 a plain
    // update() after QApplication::setPalette() leaves the OLD pixels on
    // screen (verified by on-screen pixel measurement: the palette switches
    // but the window keeps showing the previous theme, so the switch
    // "needs a second click" just to get a repaint). repaint() paints
    // synchronously with the new palette, and clearing + re-applying the
    // application stylesheet forces a full style re-polish of every widget
    // (the guard stylesheet is re-appended identically by the contrast
    // pass, so this is lossless).
    void forceCompleteRepaint()
    {
        const QString sheet = styleSheet();
        setStyleSheet(QString());
        setStyleSheet(sheet);
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *top : tops) {
            top->repaint();
            const auto kids = top->findChildren<QWidget *>();
            for (QWidget *kid : kids) {
                kid->repaint();
            }
        }
    }

    bool m_reassertDark = true;
    bool m_reasserting = false;
};

// Access the running ThemedApplication (nullptr if the QCoreApplication is not
// a ThemedApplication, e.g. the CLI mode). Uses dynamic_cast because
// ThemedApplication has no Q_OBJECT, so qobject_cast is unavailable.
inline ThemedApplication *themedApplicationInstance()
{
    return dynamic_cast<ThemedApplication *>(QCoreApplication::instance());
}

// Apply the stock dark/light preset to the running GUI app. Goes through
// ThemedApplication so the switchover re-assert state (m_reassertDark) tracks
// the user's choice. No-op if the app is not a ThemedApplication.
inline void applyStockDarkThemeToApp()
{
    if (auto *app = themedApplicationInstance()) {
        app->applyStockDarkTheme();
    }
}

inline void applyStockLightThemeToApp()
{
    if (auto *app = themedApplicationInstance()) {
        app->applyStockLightTheme();
    }
}

// True when the OS prefers dark apps (Windows registry AppsUseLightTheme,
// macOS AppleInterfaceStyle, Linux gsettings color-scheme / gtk-theme — the
// tbc-tools detection order). NOTE: the app default is DARK regardless (an
// explicit Theme-menu choice is remembered; this is kept for parity with the
// tbc-tools model and any future follow-OS option).
bool systemPrefersDark();

} // namespace ThemeUi

#endif // FLACCHOP_THEME_H
