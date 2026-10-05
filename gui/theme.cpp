#include "theme.h"

#include <QProcess>
#include <QSettings>

namespace ThemeUi {

// Cross-platform "does the OS prefer dark apps?" — the tbc-tools detection
// (src/tbc-analyse/main.cpp isDarkModeEnabled, ex ld-analyse): Windows
// registry, macOS defaults, Linux gsettings (color-scheme, then the gtk-theme
// name as a fallback).
bool systemPrefersDark()
{
#if defined(Q_OS_WIN)
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return settings.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#elif defined(Q_OS_MACOS)
    QProcess process;
    process.start(QStringLiteral("defaults"),
                  {QStringLiteral("read"), QStringLiteral("-g"),
                   QStringLiteral("AppleInterfaceStyle")});
    process.waitForFinished();
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed()
               == QStringLiteral("Dark");
#else
    QProcess process;
    process.start(QStringLiteral("gsettings"),
                  {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                   QStringLiteral("color-scheme")});
    process.waitForFinished();
    QString result = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    result.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    if (result.contains(QStringLiteral("dark"), Qt::CaseInsensitive))
        return true;
    process.start(QStringLiteral("gsettings"),
                  {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                   QStringLiteral("gtk-theme")});
    process.waitForFinished();
    result = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    result.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    return result.contains(QStringLiteral("dark"), Qt::CaseInsensitive);
#endif
}

} // namespace ThemeUi
