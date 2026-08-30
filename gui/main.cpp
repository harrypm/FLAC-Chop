#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QSize>
#include <cstdlib>
#include "mainwindow.h"

// Git-derived build version, injected by CMake (FLAC_CHOP_VERSION). Falls
// back to "dev-unknown" when built outside the CMake version step.
#ifndef FLAC_CHOP_VERSION
#define FLAC_CHOP_VERSION "dev-unknown"
#endif

// Dark Fusion palette matching ld-analyse (ld-decode/tools/ld-analyse/main.cpp)
// so FLAC-Chop visually matches the rest of the DdD/ld-decode toolset.
static void applyDarkFusion(QApplication& app)
{
    app.setStyle("Fusion");

    QPalette d;
    d.setColor(QPalette::Window, QColor(53, 53, 53));
    d.setColor(QPalette::WindowText, Qt::white);
    d.setColor(QPalette::Base, QColor(25, 25, 25));
    d.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    d.setColor(QPalette::ToolTipBase, Qt::white);
    d.setColor(QPalette::ToolTipText, Qt::white);
    d.setColor(QPalette::Text, Qt::white);
    d.setColor(QPalette::Button, QColor(53, 53, 53));
    d.setColor(QPalette::ButtonText, Qt::white);
    d.setColor(QPalette::BrightText, Qt::red);
    d.setColor(QPalette::Link, QColor(42, 130, 218));
    d.setColor(QPalette::Highlight, QColor(42, 130, 218));
    d.setColor(QPalette::HighlightedText, Qt::black);
    app.setPalette(d);
}

int main(int argc, char* argv[])
{
    // Drop any invalid QT_STYLE_OVERRIDE (e.g. "Adwaita-Dark") before
    // QApplication reads it, so Fusion + our dark palette apply cleanly.
    // Matches ld-analyse's qunsetenv approach.
    qunsetenv("QT_STYLE_OVERRIDE");

    QApplication app(argc, argv);
    app.setApplicationName("FLAC-Chop");
    app.setApplicationVersion(QStringLiteral(FLAC_CHOP_VERSION));
    app.setOrganizationName("FLAC-Chop");
    applyDarkFusion(app);
    // Multi-size QIcon so the taskbar/dock gets a crisp icon at every size
    // (16..512) instead of a scaled single raster. Matches ld-analyse's
    // multi-size icon set. On Windows prefer the .ico (multi-resolution)
    // then fall back to the PNG set.
    QIcon appIcon;
#if defined(Q_OS_WIN)
    appIcon = QIcon(":/icons/flac-chop-icon.ico");
#endif
    if (appIcon.isNull() || appIcon.availableSizes().isEmpty()) {
        appIcon = QIcon();
        const QSize sizes[] = { QSize(16,16), QSize(32,32), QSize(64,64),
                                QSize(128,128), QSize(256,256), QSize(512,512) };
        const char* paths[] = {
            ":/icons/flac-chop-icon-16.png", ":/icons/flac-chop-icon-32.png",
            ":/icons/flac-chop-icon-64.png", ":/icons/flac-chop-icon-128.png",
            ":/icons/flac-chop-icon-256.png", ":/icons/flac-chop-icon-512.png",
        };
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
            appIcon.addFile(QString::fromLatin1(paths[i]), sizes[i]);
        // Largest raster as a size-less fallback so unknown sizes still resolve.
        appIcon.addFile(":/icons/flac-chop-icon.png");
    }
    app.setWindowIcon(appIcon);

    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();

    return app.exec();
}
