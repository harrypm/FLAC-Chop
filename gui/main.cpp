#include <QApplication>
#include <QPalette>
#include <cstdlib>
#include "mainwindow.h"

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
    app.setApplicationVersion("v1.0.0");
    app.setOrganizationName("FLAC-Chop");
    applyDarkFusion(app);

    MainWindow w;
    w.show();

    return app.exec();
}
