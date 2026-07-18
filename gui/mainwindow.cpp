#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QtConcurrent>
#include <QSignalBlocker>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <cmath>
#include "rangeslider.h"

static QString ulongStr(quint64 v)
{
    // group with thousands separators for readability
    QString s = QString::number(v);
    int n = s.size();
    for (int i = n - 3; i > 0; i -= 3)
        s.insert(i, QLatin1Char(','));
    return s;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("FLAC-Chop — RF capture cutter"));
    resize(720, 580);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* title = new QLabel(tr("<h3>FLAC-Chop</h3>"
        "<div>Sample-exact FLAC cutter for RF captures "
        "(SoX engine, Rust/claxon probe) — drag a .flac in</div>"), central);
    root->addWidget(title);

    // --- Input file ---
    auto* inBox = new QGroupBox(tr("Input File"), central);
    auto* inLay = new QHBoxLayout(inBox);
    m_pathLabel = new QLabel(tr("(no file selected)"), inBox);
    m_pathLabel->setWordWrap(true);
    m_browseBtn = new QPushButton(tr("Browse..."), inBox);
    inLay->addWidget(m_pathLabel, 1);
    inLay->addWidget(m_browseBtn, 0);
    root->addWidget(inBox);

    // --- Markers: one editable time box + Set IN / Set OUT buttons ---
    // Type a time, click Set IN or Set OUT to drop that marker. The cut is
    // only ever changed by an explicit action (button or slider drag), never
    // by typing alone, so there is no textChanged -> recompute feedback loop.
    auto* markerBox = new QGroupBox(tr("Markers (real time, HH:MM:SS)"), central);
    auto* markerLay = new QGridLayout(markerBox);
    markerLay->setColumnStretch(0, 1);
    auto* timeLab = new QLabel(tr("Time:"), markerBox);
    m_timeEdit = new QLineEdit(QStringLiteral("00:00:00"), markerBox);
    m_timeEdit->setToolTip(tr("Type a time (SS, MM:SS, or HH:MM:SS), then click Set IN or Set OUT."));
    m_setInBtn = new QPushButton(tr("Set IN"), markerBox);
    m_setOutBtn = new QPushButton(tr("Set OUT"), markerBox);
    markerLay->addWidget(timeLab, 0, 0);
    markerLay->addWidget(m_timeEdit, 0, 1);
    markerLay->addWidget(m_setInBtn, 0, 2);
    markerLay->addWidget(m_setOutBtn, 0, 3);

    m_inLabel = new QLabel(QStringLiteral("--:--:--.--"), markerBox);
    m_outLabel = new QLabel(QStringLiteral("--:--:--.--"), markerBox);
    m_outLabel->setStyleSheet("color:#e8a040;");
    m_durLabel = new QLabel(QStringLiteral("--:--:--.--"), markerBox);
    auto* inRowLab = new QLabel(tr("IN:"), markerBox);
    auto* outRowLab = new QLabel(tr("OUT:"), markerBox);
    auto* durRowLab = new QLabel(tr("Duration:"), markerBox);
    markerLay->addWidget(inRowLab, 1, 0);
    markerLay->addWidget(m_inLabel, 1, 1, 1, 3);
    markerLay->addWidget(outRowLab, 2, 0);
    markerLay->addWidget(m_outLabel, 2, 1, 1, 3);
    markerLay->addWidget(durRowLab, 3, 0);
    markerLay->addWidget(m_durLabel, 3, 1, 1, 3);
    root->addWidget(markerBox);

    // --- Navigate: IN/OUT range slider (0.1 s resolution) ---
    auto* navBox = new QGroupBox(tr("Navigate — drag IN (green) / OUT (red) (0.1 s)"), central);
    auto* navLay = new QVBoxLayout(navBox);
    m_slider = new QRangeSlider(navBox);
    m_slider->setEnabled(false);
    navLay->addWidget(m_slider);
    root->addWidget(navBox);

    // --- Source info ---
    auto* infoBox = new QGroupBox(tr("Source Info (from FLAC STREAMINFO + filename)"), central);
    auto* infoLay = new QFormLayout(infoBox);
    m_headerRateLabel = new QLabel(QStringLiteral("—"), infoBox);
    m_bitsChLabel = new QLabel(QStringLiteral("—"), infoBox);
    m_mspsLabel = new QLabel(QStringLiteral("—"), infoBox);
    m_totalLabel = new QLabel(QStringLiteral("—"), infoBox);
    infoLay->addRow(tr("Header rate:"), m_headerRateLabel);
    infoLay->addRow(tr("Bits / Channels:"), m_bitsChLabel);
    infoLay->addRow(tr("MSPS (from name):"), m_mspsLabel);
    infoLay->addRow(tr("Total (real):"), m_totalLabel);
    root->addWidget(infoBox);

    // --- Preview ---
    auto* prevBox = new QGroupBox(tr("Preview"), central);
    auto* prevLay = new QFormLayout(prevBox);
    m_startSampLabel = new QLabel(QStringLiteral("—"), prevBox);
    m_lenSampLabel = new QLabel(QStringLiteral("—"), prevBox);
    m_outPathLabel = new QLabel(QStringLiteral("—"), prevBox);
    m_outPathLabel->setWordWrap(true);
    prevLay->addRow(tr("Start sample:"), m_startSampLabel);
    prevLay->addRow(tr("Length samples:"), m_lenSampLabel);
    prevLay->addRow(tr("Output file:"), m_outPathLabel);
    root->addWidget(prevBox);

    // --- Process + progress + status ---
    m_processBtn = new QPushButton(tr("Process FLAC (cut)"), central);
    m_processBtn->setEnabled(false);
    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_statusLabel = new QLabel(tr("Ready — select a FLAC file."), central);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_processBtn);
    root->addWidget(m_progress);
    root->addWidget(m_statusLabel);
    root->addStretch(1);

    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::browse);
    connect(m_processBtn, &QPushButton::clicked, this, &MainWindow::process);
    connect(m_setInBtn, &QPushButton::clicked, this, &MainWindow::setInFromBox);
    connect(m_setOutBtn, &QPushButton::clicked, this, &MainWindow::setOutFromBox);
    connect(m_slider, &QRangeSlider::inValueChanged, this, &MainWindow::onSliderInChanged);
    connect(m_slider, &QRangeSlider::outValueChanged, this, &MainWindow::onSliderOutChanged);

    // Drag & drop: the window accepts file drops; the time box must not swallow them.
    setAcceptDrops(true);
    m_timeEdit->setAcceptDrops(false);

    m_watcher = new QFutureWatcher<FcChopResult>(this);
    connect(m_watcher, &QFutureWatcher<FcChopResult>::finished,
            this, &MainWindow::onChopFinished);

    m_probeWatcher = new QFutureWatcher<FcProbe>(this);
    connect(m_probeWatcher, &QFutureWatcher<FcProbe>::finished,
            this, &MainWindow::onProbeFinished);

    if (!fc_sox_available()) {
        m_statusLabel->setText(tr("WARNING: `sox` not found on PATH — cutting will fail."));
    }
}

void MainWindow::setControlsEnabled(bool enabled)
{
    m_browseBtn->setEnabled(enabled);
    m_processBtn->setEnabled(enabled && m_probeOk && m_plan.ok);
    m_timeEdit->setEnabled(enabled);
    m_setInBtn->setEnabled(enabled && m_probeOk);
    m_setOutBtn->setEnabled(enabled && m_probeOk);
}

void MainWindow::browse()
{
    if (m_probing)
        return;
    const QString startDir = m_inPath.isEmpty() ? QDir::homePath() : QFileInfo(m_inPath).absolutePath();
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Select FLAC file"), startDir,
        tr("FLAC files (*.flac);;All files (*)"));
    if (fn.isEmpty())
        return;
    loadFile(fn);
}

void MainWindow::unloadFile()
{
    // Reset all per-file state to the unloaded defaults. Called at the top of
    // loadFile so dropping/loading a new file clears the old file's state first
    // (old IN/OUT, slider range, probe fields, plan, output path). If the new
    // probe then fails, the GUI is left cleanly unloaded instead of showing a
    // mix of old + new.
    m_probeOk = false;
    m_probe = FcProbe{};
    m_plan = FcPlan{};
    m_outPath.clear();
    m_inPath.clear();
    m_totalSec = 0.0;
    m_sliderMaxDs = 0;
    m_inSec = 0.0;
    m_outSec = 0.0;

    m_pathLabel->setText(tr("(no file selected)"));
    m_slider->setEnabled(false);
    m_slider->setRange(0, 0);
    QSignalBlocker bt(m_timeEdit);
    m_timeEdit->setText(QStringLiteral("00:00:00"));
    m_setInBtn->setEnabled(false);
    m_setOutBtn->setEnabled(false);
    m_processBtn->setEnabled(false);

    m_inLabel->setText(QStringLiteral("--:--:--.--"));
    m_outLabel->setText(QStringLiteral("--:--:--.--"));
    m_durLabel->setText(QStringLiteral("--:--:--.--"));
    m_startSampLabel->setText(QStringLiteral("—"));
    m_lenSampLabel->setText(QStringLiteral("—"));
    m_outPathLabel->setText(QStringLiteral("—"));

    setProbeInfo();
}

void MainWindow::loadFile(const QString& fn)
{
    // Clear any currently-loaded file's state before probing the new one, so a
    // failed probe (or a drop of a non-FLAC, etc.) doesn't leave a mix of old
    // and new state in the GUI.
    unloadFile();

    m_inPath = fn;
    m_pathLabel->setText(QFileInfo(fn).fileName());

    // Run the probe off the GUI thread. For files with an unknown STREAMINFO
    // total this scans every FLAC frame header (reading the whole file), which
    // can take minutes on large captures — doing it on the GUI thread would
    // freeze the window. Show a busy indicator + status while it runs.
    m_probing = true;
    setControlsEnabled(false);
    m_progress->setRange(0, 0); // busy indicator
    m_statusLabel->setText(tr("Probing… (scanning frame headers if the total is unknown)"));

    const QString path = fn;
    auto fut = QtConcurrent::run([path]() -> FcProbe {
        FcProbe r{};
        QByteArray b = path.toUtf8();
        fc_probe(b.constData(), &r);
        return r;
    });
    m_probeWatcher->setFuture(fut);
}

void MainWindow::onProbeFinished()
{
    m_probing = false;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);

    m_probe = m_probeWatcher->result();
    m_probeOk = (m_probe.ok != 0);

    if (!m_probeOk) {
        m_statusLabel->setText(tr("Probe failed: %1")
            .arg(QString::fromUtf8(m_probe.error)));
        setProbeInfo();
        m_sliderMaxDs = 0;
        m_slider->setEnabled(false);
        m_processBtn->setEnabled(false);
        m_setInBtn->setEnabled(false);
        m_setOutBtn->setEnabled(false);
        return;
    }

    setProbeInfo();

    // On load, put the IN/OUT markers at each end of the tape: IN at the
    // start (00:00:00) and OUT at the full real duration, so the slider's
    // handles sit at each end. m_inSec/m_outSec are the single source of
    // truth; we set them here, then push to the slider + time box with
    // signals blocked so no recompute fires to clobber them.
    m_syncing = true;
    if (m_probe.total_samples_known && m_totalSec > 0.0) {
        m_inSec = 0.0;
        m_outSec = m_totalSec;
    } else {
        m_inSec = 0.0;
        m_outSec = 0.0;
    }
    m_slider->setEnabled(m_sliderMaxDs > 0);
    m_slider->setRange(0, m_sliderMaxDs);
    syncSliderFromCut();
    setTimeBox(m_inSec);
    m_syncing = false;

    applyCut();
    m_statusLabel->setText(tr("Probed OK. Type a time + Set IN/OUT, or drag the slider, then Process."));
    setControlsEnabled(true);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    if (m_probing) {
        m_statusLabel->setText(tr("Already probing a file — wait for it to finish."));
        return;
    }
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QUrl u = urls.first();
    if (!u.isLocalFile())
        return;
    const QString fn = u.toLocalFile();
    if (!fn.endsWith(".flac", Qt::CaseInsensitive)) {
        m_statusLabel->setText(tr("Dropped file is not .flac: %1").arg(fn));
        return;
    }
    e->acceptProposedAction();
    loadFile(fn);
}

void MainWindow::onSliderInChanged(int v)
{
    if (m_syncing || !m_probeOk)
        return;
    m_inSec = v / 10.0;
    // keep the time box mirroring the handle being dragged so the user can
    // fine-tune it by typing afterwards; blocked so it doesn't loop back.
    setTimeBox(m_inSec);
    applyCut();
}

void MainWindow::onSliderOutChanged(int v)
{
    if (m_syncing || !m_probeOk)
        return;
    m_outSec = v / 10.0;
    setTimeBox(m_outSec);
    applyCut();
}

void MainWindow::setInFromBox()
{
    if (!m_probeOk)
        return;
    double t = 0.0;
    if (!parseHms(m_timeEdit->text(), t)) {
        m_statusLabel->setText(tr("Time not in HH:MM:SS form."));
        return;
    }
    if (t < 0.0) t = 0.0;
    // IN must stay strictly before OUT (keep at least 0.1 s span).
    if (m_outSec - t < 0.1)
        t = m_outSec - 0.1;
    m_inSec = t;
    m_syncing = true;
    syncSliderFromCut();
    m_syncing = false;
    applyCut();
}

void MainWindow::setOutFromBox()
{
    if (!m_probeOk)
        return;
    double t = 0.0;
    if (!parseHms(m_timeEdit->text(), t)) {
        m_statusLabel->setText(tr("Time not in HH:MM:SS form."));
        return;
    }
    // clamp to the tape length
    if (m_totalSec > 0.0 && t > m_totalSec) t = m_totalSec;
    // OUT must stay strictly after IN (at least 0.1 s span).
    if (t - m_inSec < 0.1)
        t = m_inSec + 0.1;
    m_outSec = t;
    m_syncing = true;
    syncSliderFromCut();
    m_syncing = false;
    applyCut();
}

void MainWindow::syncSliderFromCut()
{
    if (!m_slider)
        return;
    const int inDs = int(std::round(m_inSec * 10.0));
    const int outDs = int(std::round(m_outSec * 10.0));
    QSignalBlocker b(m_slider);
    m_slider->setRange(0, m_sliderMaxDs);
    m_slider->setInValue(inDs);
    m_slider->setOutValue(outDs);
}

void MainWindow::setTimeBox(double sec)
{
    QSignalBlocker b(m_timeEdit);
    m_timeEdit->setText(secsToHms(sec));
}

void MainWindow::setProbeInfo()
{
    if (!m_probeOk) {
        m_totalSec = 0.0;
        m_headerRateLabel->setText(QStringLiteral("—"));
        m_bitsChLabel->setText(QStringLiteral("—"));
        m_mspsLabel->setText(QStringLiteral("—"));
        m_totalLabel->setText(QStringLiteral("—"));
        return;
    }
    m_headerRateLabel->setText(tr("%1 Hz (header)").arg(m_probe.header_sample_rate));
    m_bitsChLabel->setText(tr("%1-bit / %2 ch")
        .arg(m_probe.bits_per_sample).arg(m_probe.channels));
    if (m_probe.is_rf)
        m_mspsLabel->setText(tr("RF — %1 Hz real")
            .arg(m_probe.real_rate_hz, 0, 'f', 0));
    else
        m_mspsLabel->setText(tr("audio — %1 Hz").arg(m_probe.real_rate_hz, 0, 'f', 0));
    if (m_probe.total_samples_known) {
        double realRate = m_probe.real_rate_hz;
        double totalSec = double(m_probe.total_samples) / realRate;
        m_totalSec = totalSec;
        m_sliderMaxDs = int(std::round(totalSec * 10.0));
        QString main = tr("%1 samples ≈ %2")
            .arg(ulongStr(m_probe.total_samples), secsToHms(totalSec));
        // Provenance tag (highest priority first).
        QString tag;
        if (m_probe.total_samples_from_vorbis)
            tag = tr(" (vorbis RF_TOTAL_SAMPLES)");
        else if (m_probe.total_samples_from_companion)
            tag = tr(" (companion file)");
        else if (m_probe.total_samples_scanned)
            tag = tr(" (scanned from frames)");
        else if (m_probe.total_samples_wraps > 0)
            tag = tr(" (wrap-corrected +%1×2³⁶, raw %2)")
                .arg(m_probe.total_samples_wraps)
                .arg(ulongStr(m_probe.declared_total_samples));
        if (m_probe.total_samples_estimated)
            tag += tr(" ~est.");
        if (!tag.isEmpty()) {
            m_totalLabel->setText(main + tag);
            m_totalLabel->setStyleSheet("color:#e8a040;");
        } else {
            m_totalLabel->setText(main);
            m_totalLabel->setStyleSheet("");
        }
    } else {
        m_totalSec = 0.0;
        m_sliderMaxDs = 0;
        m_totalLabel->setText(tr("unknown (no STREAMINFO total)"));
    }
}

void MainWindow::applyCut()
{
    m_plan = FcPlan{};
    m_outPath.clear();

    if (!m_probeOk) {
        m_inLabel->setText(QStringLiteral("--:--:--.--"));
        m_outLabel->setText(QStringLiteral("--:--:--.--"));
        m_durLabel->setText(QStringLiteral("--:--:--.--"));
        m_startSampLabel->setText(QStringLiteral("—"));
        m_lenSampLabel->setText(QStringLiteral("—"));
        m_outPathLabel->setText(QStringLiteral("—"));
        m_processBtn->setEnabled(false);
        return;
    }

    double startSec = m_inSec;
    double lenSec = m_outSec - m_inSec;
    if (lenSec <= 0.0) {
        m_statusLabel->setText(tr("OUT must be after IN."));
        m_processBtn->setEnabled(false);
        return;
    }

    fc_plan(startSec, lenSec, m_probe.real_rate_hz,
            m_probe.total_samples, m_probe.total_samples_known, &m_plan);

    if (!m_plan.ok) {
        m_statusLabel->setText(tr("Plan error: %1")
            .arg(QString::fromUtf8(m_plan.error)));
        m_processBtn->setEnabled(false);
        return;
    }

    // output path via the Rust helper
    char buf[4096];
    if (fc_generate_output_path(m_inPath.toUtf8().constData(), buf, sizeof(buf))) {
        m_outPath = QString::fromUtf8(buf);
    } else {
        m_outPath = m_inPath + QStringLiteral("-cut.flac");
    }

    m_inLabel->setText(secsToHms(m_inSec));
    m_outLabel->setText(secsToHms(m_outSec));
    m_durLabel->setText(secsToHms(lenSec));
    m_startSampLabel->setText(tr("%1  (@ %2 Hz)")
        .arg(ulongStr(m_plan.start_samples))
        .arg(m_plan.real_sample_rate_hz, 0, 'f', 0));
    m_lenSampLabel->setText(ulongStr(m_plan.length_samples));
    m_outPathLabel->setText(m_outPath);
    m_processBtn->setEnabled(true);
    m_statusLabel->setText(tr("Plan ready: %1 + %2 samples.")
        .arg(ulongStr(m_plan.start_samples), ulongStr(m_plan.length_samples)));
}

void MainWindow::process()
{
    if (!m_probeOk || !m_plan.ok || m_outPath.isEmpty())
        return;

    if (QFile::exists(m_outPath)) {
        auto r = QMessageBox::question(this, tr("Overwrite?"),
            tr("Output file exists:\n%1\nOverwrite?").arg(m_outPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes)
            return;
    }

    setControlsEnabled(false);
    m_progress->setRange(0, 0); // busy indicator
    m_statusLabel->setText(tr("Processing... sox trim %1s %2s")
        .arg(ulongStr(m_plan.start_samples), ulongStr(m_plan.length_samples)));

    const QString inPath = m_inPath;
    const QString outPath = m_outPath;
    const quint64 start = m_plan.start_samples;
    const quint64 len = m_plan.length_samples;

    auto fut = QtConcurrent::run([inPath, outPath, start, len]() -> FcChopResult {
        FcChopResult r{};
        QByteArray inB = inPath.toUtf8();
        QByteArray outB = outPath.toUtf8();
        fc_chop(inB.constData(), outB.constData(), start, len, &r);
        return r;
    });
    m_watcher->setFuture(fut);
}

void MainWindow::onChopFinished()
{
    FcChopResult r = m_watcher->result();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);

    if (r.ok) {
        m_statusLabel->setText(tr("Done. Output: %1").arg(m_outPath));
    } else {
        QString err = QString::fromUtf8(r.stderr_buf).trimmed();
        if (err.isEmpty())
            err = tr("(no stderr) sox exit code %1").arg(r.exit_code);
        m_statusLabel->setText(tr("FAILED (exit %1): %2").arg(r.exit_code).arg(err));
        QMessageBox::warning(this, tr("Cut failed"),
            tr("sox failed (exit %1):\n%2").arg(r.exit_code).arg(err));
    }

    setControlsEnabled(true);
}

bool MainWindow::parseHms(const QString& s, double& outSec)
{
    QString t = s.trimmed();
    if (t.isEmpty())
        return false;
    const auto parts = t.split(QLatin1Char(':'));
    double h = 0.0, m = 0.0, sec = 0.0;
    bool ok = false;
    if (parts.size() == 1) {
        sec = parts[0].toDouble(&ok);
    } else if (parts.size() == 2) {
        m = parts[0].toDouble(&ok);
        if (ok) sec = parts[1].toDouble(&ok);
    } else if (parts.size() == 3) {
        h = parts[0].toDouble(&ok);
        if (ok) m = parts[1].toDouble(&ok);
        if (ok) sec = parts[2].toDouble(&ok);
    } else {
        return false;
    }
    if (!ok)
        return false;
    if (h < 0.0 || m < 0.0 || m >= 60.0 || sec < 0.0 || sec >= 60.0)
        return false;
    outSec = h * 3600.0 + m * 60.0 + sec;
    return true;
}

QString MainWindow::secsToHms(double s)
{
    if (s < 0.0)
        s = 0.0;
    int whole = int(std::floor(s));
    int h = whole / 3600;
    int m = (whole % 3600) / 60;
    int sec = whole % 60;
    int ms = int(std::round((s - whole) * 1000.0));
    if (ms == 1000) { ms = 0; sec++; if (sec == 60) { sec = 0; m++; if (m == 60) { m = 0; h++; } } }
    return QString::asprintf("%02d:%02d:%02d.%03d", h, m, sec, ms);
}
