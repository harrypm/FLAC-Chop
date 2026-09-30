#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QSize>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
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

// --- GUI launch forms ------------------------------------------------------
// Two argument forms launch the GUI instead of the headless CLI:
//   flac-chop --gui [<file>] [--in <pos>] [--out <pos>] [--units samples|seconds]
//   flac-chop <file>                        (a single positional file arg)
// The GUI opens with the file loaded and the IN/OUT markers set from --in/--out
// (positions in real seconds, or exact real RF samples with --units samples —
// the same units as vhs-decode metadata fileLoc). No window opens for any
// other argument form (the headless CLI below handles those).
struct GuiLaunch {
    QString file;               // may be empty (plain --gui)
    double inPos = std::numeric_limits<double>::quiet_NaN();  // --in  (NaN = not given)
    double outPos = std::numeric_limits<double>::quiet_NaN(); // --out (NaN = not given)
    bool unitsSamples = false;  // --units samples|seconds (default seconds)
};

// Parse the --gui argument list (args[1] == "--gui", scan from args[2]).
// Returns false (with a message on stderr) on a usage error.
static bool parseGuiArgs(const QStringList& args, GuiLaunch& gl)
{
    for (int i = 2; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == QStringLiteral("--in") && i + 1 < args.size()) {
            bool ok = false;
            gl.inPos = args[++i].toDouble(&ok);
            if (!ok || gl.inPos < 0.0) {
                std::fprintf(stderr, "--in must be a non-negative number\n");
                return false;
            }
        } else if (a == QStringLiteral("--out") && i + 1 < args.size()) {
            bool ok = false;
            gl.outPos = args[++i].toDouble(&ok);
            if (!ok || gl.outPos < 0.0) {
                std::fprintf(stderr, "--out must be a non-negative number\n");
                return false;
            }
        } else if (a == QStringLiteral("--units") && i + 1 < args.size()) {
            const QString u = args[++i];
            if (u == QStringLiteral("samples")) {
                gl.unitsSamples = true;
            } else if (u == QStringLiteral("seconds")) {
                gl.unitsSamples = false;
            } else {
                std::fprintf(stderr, "--units must be 'samples' or 'seconds'\n");
                return false;
            }
        } else if (!a.startsWith(QLatin1String("--")) && gl.file.isEmpty()) {
            gl.file = a;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", qPrintable(a));
            return false;
        }
    }
    return true;
}

// Decide what a given argument vector means:
//   0 = run the headless CLI (runCli)
//   1 = launch the GUI (gl holds the pre-load request when activateGui)
//   2 = usage error (message already printed)
// activateGui is set when the caller must pass the launch to MainWindow::
// loadFileAndMarkers (an empty file = plain GUI).
static int detectLaunchMode(const QStringList& args, GuiLaunch& gl, bool& activateGui)
{
    activateGui = false;
    if (args.size() == 1)
        return 1; // no args: the GUI as today
    // --gui [<file>] [--in <pos>] [--out <pos>] [--units samples|seconds]
    if (args[1] == QStringLiteral("--gui")) {
        if (!parseGuiArgs(args, gl))
            return 2;
        activateGui = true;
        return 1;
    }
    // A single positional argument that is not a flag: a file to pre-load.
    if (args.size() == 2 && !args[1].startsWith(QLatin1String("--"))) {
        gl.file = args[1];
        activateGui = true;
        return 1;
    }
    return 0;
}

// --- CLI mode -------------------------------------------------------------
// When the binary is run with arguments, run headless (no QApplication / GUI):
// probe -> plan -> SoX chop -> rename + RF tag rewrite, printing a concise
// report. This reuses the exact FFI path the GUI uses, so it doubles as a
// smoke/automation harness for the whole cut pipeline on real RF captures.
//
// Usage:
//   flac-chop <in> <out.flac|outDir> <start> <len>
//            [--units samples|seconds] [--rate 10000|16000|20000|24000|28600]
//            [--bits 8|12|6] [--no-filter]
//   flac-chop --probe <in> [--json]
//   flac-chop --gui [<file>] [--in <pos>] [--out <pos>] [--units samples|seconds]
//   flac-chop --version
//
// Inputs: FLAC (.flac + fLaC-magic files), Ogg FLAC (.ldf/.oga/.ogg — the
// real vhs-decode ld-compress output, detected by the OggS magic), PCM WAV, and
// headerless raw PCM (.u8/.u16/.s8/.s16/.r8/.r16 and the reversed
// .8u/.8s/.16u/.16s; .raw/.bin/.pcm assumed u8). Raw files must carry the rate
// in their name (e.g. ..._8-bit_20msps.u8).
//
// <out> may be a full output path OR a directory (the renamed stem is then
// derived from the input name + the chosen rate/bits, matching the GUI).
// With no args, the GUI launches as normal.
// --- probe output -----------------------------------------------------------

// Print the probe result as JSON with stable snake_case keys covering every
// FcProbe field. QJsonObject sorts keys alphabetically, so the output order is
// stable too. Works for a failed probe as well (ok=false + error). Returns the
// process exit code (0 = probed ok, 1 = probe failed).
static int printProbeJson(const FcProbe& p)
{
    static const char* kFmtNames[] = { "flac", "wav", "raw u8", "raw s8", "raw u16", "raw s16", "ogg-flac" };
    QJsonObject o;
    o.insert(QStringLiteral("ok"), p.ok != 0);
    o.insert(QStringLiteral("error"), QString::fromUtf8(p.error));
    o.insert(QStringLiteral("format"), QString::fromLatin1(p.format <= 6 ? kFmtNames[p.format] : "?"));
    o.insert(QStringLiteral("format_code"), int(p.format));
    o.insert(QStringLiteral("header_sample_rate"), double(p.header_sample_rate));
    o.insert(QStringLiteral("declared_total_samples"), double(p.declared_total_samples));
    o.insert(QStringLiteral("total_samples"), double(p.total_samples));
    o.insert(QStringLiteral("total_samples_known"), p.total_samples_known != 0);
    o.insert(QStringLiteral("total_samples_wraps"), int(p.total_samples_wraps));
    o.insert(QStringLiteral("total_samples_estimated"), p.total_samples_estimated != 0);
    o.insert(QStringLiteral("total_samples_scanned"), p.total_samples_scanned != 0);
    o.insert(QStringLiteral("total_samples_from_companion"), p.total_samples_from_companion != 0);
    o.insert(QStringLiteral("total_samples_from_vorbis"), p.total_samples_from_vorbis != 0);
    o.insert(QStringLiteral("total_samples_from_ogg"), p.total_samples_from_ogg != 0);
    o.insert(QStringLiteral("rate_from_vorbis"), p.rate_from_vorbis != 0);
    o.insert(QStringLiteral("bits_per_sample"), int(p.bits_per_sample));
    o.insert(QStringLiteral("channels"), int(p.channels));
    o.insert(QStringLiteral("file_size"), double(p.file_size));
    o.insert(QStringLiteral("audio_offset"), double(p.audio_offset));
    o.insert(QStringLiteral("real_rate_hz"), double(p.real_rate_hz));
    o.insert(QStringLiteral("is_rf"), p.is_rf != 0);
    o.insert(QStringLiteral("msps"), double(p.msps));
    o.insert(QStringLiteral("msps_known"), p.msps_known != 0);
    // The probe packs diagnostics "; "-joined; expose them as a JSON array.
    QJsonArray warnings;
    const QString w = QString::fromUtf8(p.warnings);
    if (!w.isEmpty()) {
        for (const QString& part : w.split(QStringLiteral("; "), Qt::SkipEmptyParts))
            warnings.append(part.trimmed());
    }
    o.insert(QStringLiteral("warnings"), warnings);
    std::printf("%s\n",
                QJsonDocument(o).toJson(QJsonDocument::Indented).constData());
    return p.ok ? 0 : 1;
}

static int runCli(int argc, char* argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    const QStringList args = QCoreApplication::arguments();
    // --version
    if (args.size() == 2 && args[1] == QStringLiteral("--version")) {
        std::printf("FLAC-Chop %s\n", FLAC_CHOP_VERSION);
        return 0;
    }
    // --probe <file> [--json]
    if (args.size() >= 3 && args[1] == QStringLiteral("--probe")) {
        bool json = false;
        for (int i = 3; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("--json"))
                json = true;
            else { std::fprintf(stderr, "unknown arg: %s\n", qPrintable(args[i])); return 2; }
        }
        FcProbe p{};
        const QByteArray pb = args[2].toUtf8();
        fc_probe(pb.constData(), &p);
        if (json)
            return printProbeJson(p);
        if (!p.ok) { std::fprintf(stderr, "probe error: %s\n", p.error); return 1; }
        static const char* kFmtNames[] = { "flac", "wav", "raw u8", "raw s8", "raw u16", "raw s16", "ogg-flac" };
        std::printf("ok                 : true\n");
        std::printf("format             : %s\n",
                    p.format <= 6 ? kFmtNames[p.format] : "?");
        std::printf("header_sample_rate : %llu Hz\n", (unsigned long long)p.header_sample_rate);
        std::printf("bits_per_sample    : %u\n", p.bits_per_sample);
        std::printf("channels           : %u\n", p.channels);
        std::printf("real_rate_hz       : %.0f  (is_rf=%d)\n", p.real_rate_hz, p.is_rf);
        std::printf("declared_total     : %llu (STREAMINFO)\n", (unsigned long long)p.declared_total_samples);
        std::printf("total_samples      : %llu (real)\n", (unsigned long long)p.total_samples);
        std::printf("total_known        : %d%s\n", p.total_samples_known,
                    p.total_samples_from_ogg ? " (exact, from the Ogg stream)" : "");
        std::printf("warnings           : %s\n", p.warnings[0] ? p.warnings : "(none)");
        return 0;
    }
    // chop: <in> <out|dir> <start> <len> [opts]
    if (args.size() >= 5 && args[1] != QStringLiteral("--help") && args[1] != QStringLiteral("-h")) {
        const QString inPath = args[1];
        QString outArg = args[2];
        // optional flags — pre-scanned first so --units governs how the
        // positional start/len are parsed.
        quint64 outRateHz = 0; uint outBits = 0; bool basicFilter = true;
        bool samplesUnits = false; // default: seconds (back-compat)
        for (int i = 5; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("--rate") && i + 1 < args.size())
                outRateHz = args[++i].toULongLong();
            else if (args[i] == QStringLiteral("--bits") && i + 1 < args.size()) {
                outBits = args[++i].toUInt();
                if (outBits != 8 && outBits != 12 && outBits != 6) {
                    std::fprintf(stderr, "--bits must be 8, 12, or 6\n");
                    return 2;
                }
            }
            else if (args[i] == QStringLiteral("--units") && i + 1 < args.size()) {
                const QString u = args[++i];
                if (u == QStringLiteral("samples")) {
                    samplesUnits = true;
                } else if (u == QStringLiteral("seconds")) {
                    samplesUnits = false;
                } else {
                    std::fprintf(stderr, "--units must be 'samples' or 'seconds'\n");
                    return 2;
                }
            }
            else if (args[i] == QStringLiteral("--no-filter"))
                basicFilter = false;
            else { std::fprintf(stderr, "unknown arg: %s\n", qPrintable(args[i])); return 2; }
        }
        // Positional start/len: exact integer real RF samples in --units
        // samples mode, real seconds otherwise (default, back-compat).
        bool ok1 = false, ok2 = false;
        quint64 startSamp = 0, lenSamp = 0;
        double startSec = 0.0, lenSec = 0.0;
        if (samplesUnits) {
            startSamp = args[3].toULongLong(&ok1);
            lenSamp = args[4].toULongLong(&ok2);
            if (!ok1 || !ok2) {
                std::fprintf(stderr, "start/len must be non-negative integer sample counts in --units samples mode\n");
                return 2;
            }
        } else {
            startSec = args[3].toDouble(&ok1);
            lenSec = args[4].toDouble(&ok2);
            if (!ok1 || !ok2) { std::fprintf(stderr, "start/len not numbers (seconds, or integer samples with --units samples)\n"); return 2; }
        }
        // probe
        FcProbe p{};
        const QByteArray inB = inPath.toUtf8();
        fc_probe(inB.constData(), &p);
        if (!p.ok) { std::fprintf(stderr, "probe error: %s\n", p.error); return 1; }
        // plan. In samples mode start/len are exact real RF sample counts —
        // the same units as vhs-decode metadata fileLoc (real RF sample
        // index) — passed 1:1 to fc_chop, clamped to the probed total exactly
        // like fc_plan clamps second-derived counts. In seconds mode fc_plan
        // computes the counts from the real rate.
        FcPlan plan{};
        if (samplesUnits) {
            if (p.total_samples_known && startSamp >= p.total_samples) {
                std::fprintf(stderr, "plan error: start is at or past the end of the file\n");
                return 1;
            }
            if (lenSamp == 0) { std::fprintf(stderr, "length must be > 0\n"); return 2; }
            if (p.total_samples_known && lenSamp > p.total_samples - startSamp)
                lenSamp = p.total_samples - startSamp; // clamp to the file end
            plan.ok = 1;
            plan.start_samples = startSamp;
            plan.length_samples = lenSamp;
            plan.end_sample = startSamp + lenSamp;
            plan.real_sample_rate_hz = p.real_rate_hz;
        } else {
            fc_plan(startSec, lenSec, p.real_rate_hz,
                    p.total_samples, p.total_samples_known, &plan);
            if (!plan.ok) { std::fprintf(stderr, "plan error: %s\n", plan.error); return 1; }
        }
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
        std::printf("plan: start=%llu len=%llu samples%s (header_rate %llu Hz, is_rf=%d) -> %s\n",
                     (unsigned long long)plan.start_samples,
                     (unsigned long long)plan.length_samples,
                     samplesUnits ? QStringLiteral(" (exact, --units samples)").toUtf8().constData() : "",
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
        "GUI:   flac-chop                  (no args -> launch the GUI)\n"
        "       flac-chop <file>           (launch the GUI with <file> loaded)\n"
        "       flac-chop --gui [<file>] [--in <pos>] [--out <pos>]\
"
        "                 [--units samples|seconds]\n\n"
        "CLI:\n"
        "  flac-chop <in.flac> <out.flac|dir> <start> <len>\
"
        "          [--units samples|seconds] [--rate 10000|16000|20000|24000|28600]\n"
        "          [--bits 8|12|6] [--no-filter]\n"
        "  flac-chop --probe <in.flac> [--json]\n"
        "  flac-chop --version\n\n"
        "Units: --units seconds (default) takes start/len as real seconds;\
"
        "       --units samples takes them as exact real RF sample counts (the\
"
        "       same units as vhs-decode metadata fileLoc).\n\n"
        "Exit codes: 0 = ok, 1 = pipeline error (reason on stderr), 2 = usage error.\n",
        FLAC_CHOP_VERSION);
    return args.size() == 1 ? 0 : 2;
}

int main(int argc, char* argv[])
{
    // Drop any invalid QT_STYLE_OVERRIDE (e.g. "Adwaita-Dark") before
    // QApplication reads it, so Fusion + our dark palette apply cleanly.
    // Matches ld-analyse's qunsetenv approach.
    qunsetenv("QT_STYLE_OVERRIDE");

    // Launch routing (see detectLaunchMode):
    //   no args, --gui [...], or a single positional <file> -> the GUI
    //   (with the file/markers pre-loaded when given);
    //   anything else -> the headless CLI (runCli) via QCoreApplication.
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);
    GuiLaunch gui;
    bool activateGui = false;
    const int mode = detectLaunchMode(rawArgs, gui, activateGui);
    if (mode == 2)
        return 2; // --gui usage error (message already on stderr)
    if (mode == 0) {
        // CLI mode: run headless via QCoreApplication (no GUI). This makes
        // the binary scriptable + automatable and gives a fast smoke path
        // for the whole cut pipeline on real RF captures.
        QCoreApplication cliApp(argc, argv);
        cliApp.setApplicationName(QStringLiteral("FLAC-Chop"));
        cliApp.setApplicationVersion(QStringLiteral(FLAC_CHOP_VERSION));
        return runCli(argc, argv);
    }

#if defined(Q_OS_LINUX)
    // Linux taskbar identity. Qt derives the X11 WM_CLASS instance name from
    // argv[0], which inside an AppImage is a temporary mount path. Pin it to
    // "flac-chop" (the .desktop file basename) unless the caller set one.
    if (!qEnvironmentVariableIsSet("RESOURCE_NAME"))
        qputenv("RESOURCE_NAME", "flac-chop");
#endif

    QApplication app(argc, argv);
    app.setApplicationName("FLAC-Chop");
    app.setApplicationVersion(QStringLiteral(FLAC_CHOP_VERSION));
    app.setOrganizationName("FLAC-Chop");
#if defined(Q_OS_LINUX)
    // WM_CLASS class = applicationName ("FLAC-Chop") matches
    // StartupWMClass=FLAC-Chop in flac-chop.desktop; the desktop file name
    // gives the Wayland app_id / freedesktop association. Both must be set
    // before the first window is shown (Qt reads them at native window
    // creation), which happens at w.show() below.
    QGuiApplication::setDesktopFileName(QStringLiteral("flac-chop"));
#endif
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
    if (activateGui && !gui.file.isEmpty())
        w.loadFileAndMarkers(gui.file, gui.inPos, gui.outPos, gui.unitsSamples);

    return app.exec();
}
