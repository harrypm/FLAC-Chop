#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QPalette>
#include <QSize>
#include <QFileInfo>
#include <QRegularExpression>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "mainwindow.h"
#include "flacchop.h"

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

// --- CLI mode -------------------------------------------------------------
// When the binary is run with arguments, run headless (no QApplication / GUI):
// probe -> plan -> SoX chop -> rename + RF tag rewrite, printing a concise
// report. This reuses the exact FFI path the GUI uses, so it doubles as a
// smoke/automation harness for the whole cut pipeline on real RF captures.
//
// Usage:
//   flac-chop <in> <out.flac|outDir> <start_sec> <len_sec>
//            [--rate 16000|20000|24000|28600] [--bits 8|6]
//            [--no-filter]
//   flac-chop --probe <in>
//   flac-chop --version
//
// Inputs: FLAC (.flac/.ldf + fLaC-magic files), PCM WAV, and headerless raw
// PCM (.u8/.u16/.s8/.s16/.r8/.r16; .raw/.bin assumed u8). Raw files must
// carry the rate in their name (e.g. ..._8-bit_20msps.u8).
//
// <out> may be a full output path OR a directory (the renamed stem is then
// derived from the input name + the chosen rate/bits, matching the GUI).
// With no args, the GUI launches as normal.
static int runCli(int argc, char* argv[])
{
    const QStringList args = QCoreApplication::arguments();
    // --version
    if (args.size() == 2 && args[1] == QStringLiteral("--version")) {
        std::printf("FLAC-Chop %s\n", FLAC_CHOP_VERSION);
        return 0;
    }
    // --probe <file>
    if (args.size() == 3 && args[1] == QStringLiteral("--probe")) {
        FcProbe p{};
        const QByteArray pb = args[2].toUtf8();
        fc_probe(pb.constData(), &p);
        if (!p.ok) { std::fprintf(stderr, "probe error: %s\n", p.error); return 1; }
        static const char* kFmtNames[] = { "flac", "wav", "raw u8", "raw s8", "raw u16", "raw s16" };
        std::printf("ok                 : true\n");
        std::printf("format             : %s\n",
                    p.format <= 5 ? kFmtNames[p.format] : "?");
        std::printf("header_sample_rate : %llu Hz\n", (unsigned long long)p.header_sample_rate);
        std::printf("bits_per_sample    : %u\n", p.bits_per_sample);
        std::printf("channels           : %u\n", p.channels);
        std::printf("real_rate_hz       : %.0f  (is_rf=%d)\n", p.real_rate_hz, p.is_rf);
        std::printf("declared_total     : %llu (STREAMINFO)\n", (unsigned long long)p.declared_total_samples);
        std::printf("total_samples      : %llu (real)\n", (unsigned long long)p.total_samples);
        std::printf("total_known        : %d\n", p.total_samples_known);
        std::printf("warnings           : %s\n", p.warnings[0] ? p.warnings : "(none)");
        return 0;
    }
    // chop: <in> <out|dir> <start> <len> [opts]
    if (args.size() >= 5 && args[1] != QStringLiteral("--help") && args[1] != QStringLiteral("-h")) {
        const QString inPath = args[1];
        QString outArg = args[2];
        bool ok1 = false, ok2 = false;
        const double startSec = args[3].toDouble(&ok1);
        const double lenSec = args[4].toDouble(&ok2);
        if (!ok1 || !ok2) { std::fprintf(stderr, "start_sec/len_sec not numbers\n"); return 2; }
        // optional flags
        quint64 outRateHz = 0; uint outBits = 0; bool basicFilter = true;
        for (int i = 5; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("--rate") && i + 1 < args.size())
                outRateHz = args[++i].toULongLong();
            else if (args[i] == QStringLiteral("--bits") && i + 1 < args.size())
                outBits = args[++i].toUInt();
            else if (args[i] == QStringLiteral("--no-filter"))
                basicFilter = false;
            else { std::fprintf(stderr, "unknown arg: %s\n", qPrintable(args[i])); return 2; }
        }
        // probe
        FcProbe p{};
        const QByteArray inB = inPath.toUtf8();
        fc_probe(inB.constData(), &p);
        if (!p.ok) { std::fprintf(stderr, "probe error: %s\n", p.error); return 1; }
        // plan with STREAMINFO values (SoX reads at the on-disk rate)
        FcPlan plan{};
        fc_plan(startSec, lenSec, p.real_rate_hz,
                p.total_samples, p.total_samples_known, &plan);
        if (!plan.ok) { std::fprintf(stderr, "plan error: %s\n", plan.error); return 1; }
        // resolve output path: if outArg is a dir (or doesn't end in .flac),
        // generate via fc_generate_output_path with a renamed stem.
        QString outPath;
        const bool outIsDir = QFileInfo(outArg).isDir() || !outArg.endsWith(QStringLiteral(".flac"), Qt::CaseInsensitive);
        if (outIsDir) {
            // renamed stem reflecting the new rate/bits (MISRC convention)
            // MISRC convention (gui_settings.c): <base>_<B>-bit_<N>msps
            QString stem;
            const QString inStem = QFileInfo(inPath).completeBaseName();
            static const QRegularExpression re(QStringLiteral("^(.*?)([0-9]+)-bit_([0-9]+)msps(.*)$"));
            const auto m = re.match(inStem);
            if (m.hasMatch()) {
                const QString bitsTok = (outBits > 0) ? QString::number(outBits) : m.captured(2);
                const QString mspsTok = (outRateHz > 0) ? QString::number(outRateHz / 1000) : m.captured(3);
                stem = m.captured(1) + bitsTok + QStringLiteral("-bit_") + mspsTok + QStringLiteral("msps") + m.captured(4);
            }
            char buf[4096];
            const QByteArray stemB = stem.toUtf8();
            const QByteArray dirB = outArg.toUtf8();
            if (fc_generate_output_path(inB.constData(), dirB.constData(), stemB.constData(), buf, sizeof(buf)))
                outPath = QString::fromUtf8(buf);
            else
                outPath = outArg + QStringLiteral("/") + QFileInfo(inPath).completeBaseName() + QStringLiteral("-cut.flac");
        } else {
            outPath = outArg;
        }
        std::printf("plan: start=%llu len=%llu samples (header_rate %llu Hz, is_rf=%d) -> %s\n",
                     (unsigned long long)plan.start_samples,
                     (unsigned long long)plan.length_samples,
                     (unsigned long long)p.header_sample_rate, p.is_rf, qPrintable(outPath));
        // chop (blocking) — reuses the GUI's fc_chop (tag rewrite + rename)
        FcChopResult r{};
        const QByteArray outB = outPath.toUtf8();
        const qint32 basic = (outRateHz > 0 && basicFilter) ? 1 : 0;
        const qint32 isRf = p.is_rf;
        fc_chop(inB.constData(), outB.constData(), plan.start_samples, plan.length_samples,
                outRateHz, outBits, basic, isRf, &r);
        if (r.ok) {
            std::printf("ok: wrote %s\n", qPrintable(outPath));
            if (r.stderr_buf[0]) std::printf("  note: %s\n", r.stderr_buf);
            return 0;
        }
        std::fprintf(stderr, "sox failed (exit %d): %s\n", r.exit_code, r.stderr_buf);
        return 1;
    }
    // --help / -h / unknown
    std::printf(
        "FLAC-Chop %s — sample-exact RF FLAC cutter\n\n"
        "GUI:   flac-chop                  (no args -> launch the GUI)\n\n"
        "CLI:\n"
        "  flac-chop <in.flac> <out.flac|dir> <start_sec> <len_sec>\
"
        "          [--rate 16000|20000|24000|28600] [--bits 8|6] [--no-filter]\n"
        "  flac-chop --probe <in.flac>\n"
        "  flac-chop --version\n",
        FLAC_CHOP_VERSION);
    return args.size() == 1 ? 0 : 2;
}

int main(int argc, char* argv[])
{
    // Drop any invalid QT_STYLE_OVERRIDE (e.g. "Adwaita-Dark") before
    // QApplication reads it, so Fusion + our dark palette apply cleanly.
    // Matches ld-analyse's qunsetenv approach.
    qunsetenv("QT_STYLE_OVERRIDE");

    // CLI mode: if any args are present, run headless via QCoreApplication
    // (no GUI). This makes the binary scriptable + automatable and gives a
    // fast smoke path for the whole cut pipeline on real RF captures.
    if (argc > 1) {
        QCoreApplication cliApp(argc, argv);
        cliApp.setApplicationName(QStringLiteral("FLAC-Chop"));
        cliApp.setApplicationVersion(QStringLiteral(FLAC_CHOP_VERSION));
        return runCli(argc, argv);
    }

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
