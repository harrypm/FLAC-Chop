#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
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
    resize(720, 560);

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

    // --- Time parameters ---
    auto* timeBox = new QGroupBox(tr("Time (real seconds, HH:MM:SS)"), central);
    auto* timeLay = new QFormLayout(timeBox);
    m_inEdit = new QLineEdit(QStringLiteral("00:00:00"), timeBox);
    m_lenEdit = new QLineEdit(QStringLiteral("00:00:10"), timeBox);
    m_outLabel = new QLabel(QStringLiteral("--:--:--"), timeBox);
    m_outLabel->setStyleSheet("color:#e8a040;");
    timeLay->addRow(tr("IN (start):"), m_inEdit);
    timeLay->addRow(tr("Duration:"), m_lenEdit);
    timeLay->addRow(tr("OUT (computed):"), m_outLabel);
    root->addWidget(timeBox);

    // --- Navigate: IN/OUT range slider (0.1 s resolution) ---
    auto* navBox = new QGroupBox(tr("Navigate — drag IN / OUT (0.1 s)"), central);
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
    connect(m_inEdit, &QLineEdit::textChanged, this, &MainWindow::recompute);
    connect(m_lenEdit, &QLineEdit::textChanged, this, &MainWindow::recompute);
    connect(m_slider, &QRangeSlider::inValueChanged, this, &MainWindow::onSliderInChanged);
    connect(m_slider, &QRangeSlider::outValueChanged, this, &MainWindow::onSliderOutChanged);

    // Drag & drop: the window accepts file drops; line edits must not swallow them.
    setAcceptDrops(true);
    m_inEdit->setAcceptDrops(false);
    m_lenEdit->setAcceptDrops(false);

    m_watcher = new QFutureWatcher<FcChopResult>(this);
    connect(m_watcher, &QFutureWatcher<FcChopResult>::finished,
            this, &MainWindow::onChopFinished);

    if (!fc_sox_available()) {
        m_statusLabel->setText(tr("WARNING: `sox` not found on PATH — cutting will fail."));
    }
}

void MainWindow::setControlsEnabled(bool enabled)
{
    m_browseBtn->setEnabled(enabled);
    m_processBtn->setEnabled(enabled && m_probeOk && m_plan.ok);
    m_inEdit->setEnabled(enabled);
    m_lenEdit->setEnabled(enabled);
}

void MainWindow::browse()
{
    const QString startDir = m_inPath.isEmpty() ? QDir::homePath() : QFileInfo(m_inPath).absolutePath();
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Select FLAC file"), startDir,
        tr("FLAC files (*.flac);;All files (*)"));
    if (fn.isEmpty())
        return;
    loadFile(fn);
}

void MainWindow::loadFile(const QString& fn)
{
    m_inPath = fn;
    m_pathLabel->setText(QFileInfo(fn).fileName());

    m_probe = FcProbe{};
    fc_probe(fn.toUtf8().constData(), &m_probe);
    m_probeOk = (m_probe.ok != 0);

    if (!m_probeOk) {
        m_statusLabel->setText(tr("Probe failed: %1")
            .arg(QString::fromUtf8(m_probe.error)));
        setProbeInfo();
        m_sliderMaxDs = 0;
        m_slider->setEnabled(false);
        m_processBtn->setEnabled(false);
        return;
    }

    setProbeInfo();
    recompute();
    m_statusLabel->setText(tr("Probed OK. Drag the slider or edit IN/Duration, then Process."));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
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
    const double startSec = v / 10.0;
    QSignalBlocker b(m_inEdit);
    m_inEdit->setText(secsToHms(startSec));
    recompute();
}

void MainWindow::onSliderOutChanged(int v)
{
    if (m_syncing || !m_probeOk)
        return;
    const double outSec = v / 10.0;
    double startSec = 0.0;
    if (!parseHms(m_inEdit->text(), startSec))
        return;
    double lenSec = outSec - startSec;
    if (lenSec < 0.1)
        lenSec = 0.1;
    QSignalBlocker b(m_lenEdit);
    m_lenEdit->setText(secsToHms(lenSec));
    recompute();
}

void MainWindow::syncSliderFromEdits()
{
    if (!m_slider)
        return;
    double startSec = 0.0, lenSec = 0.0;
    if (!m_probeOk || !parseHms(m_inEdit->text(), startSec)
        || !parseHms(m_lenEdit->text(), lenSec))
    {
        m_slider->setEnabled(false);
        return;
    }
    m_slider->setEnabled(m_sliderMaxDs > 0);
    m_slider->setRange(0, m_sliderMaxDs);
    const int inDs = int(std::round(startSec * 10.0));
    const int outDs = int(std::round((startSec + lenSec) * 10.0));
    QSignalBlocker b(m_slider);
    m_slider->setInValue(inDs);
    m_slider->setOutValue(outDs);
}

void MainWindow::setProbeInfo()
{
    if (!m_probeOk) {
        m_headerRateLabel->setText(QStringLiteral("—"));
        m_bitsChLabel->setText(QStringLiteral("—"));
        m_mspsLabel->setText(QStringLiteral("—"));
        m_totalLabel->setText(QStringLiteral("—"));
        return;
    }
    m_headerRateLabel->setText(tr("%1 Hz (header)").arg(m_probe.header_sample_rate));
    m_bitsChLabel->setText(tr("%1-bit / %2 ch")
        .arg(m_probe.bits_per_sample).arg(m_probe.channels));
    if (m_probe.msps_known)
        m_mspsLabel->setText(tr("%1 MSPS (from filename)").arg(m_probe.msps, 0, 'f', 3));
    else
        m_mspsLabel->setText(tr("(none in filename)"));
    if (m_probe.total_samples_known) {
        double realRate = (m_probe.msps_known && m_probe.msps > 0.0)
            ? m_probe.msps * 1e6 : double(m_probe.header_sample_rate);
        double totalSec = double(m_probe.total_samples) / realRate;
        m_sliderMaxDs = int(std::round(totalSec * 10.0));
        QString main = tr("%1 samples ≈ %2")
            .arg(ulongStr(m_probe.total_samples), secsToHms(totalSec));
        if (m_probe.total_samples_wraps > 0) {
            QString tag = tr(" (wrap-corrected +%1×2³⁶, raw %2)")
                .arg(m_probe.total_samples_wraps)
                .arg(ulongStr(m_probe.declared_total_samples));
            if (m_probe.total_samples_estimated)
                tag += tr(" ~est.");
            m_totalLabel->setText(main + tag);
            m_totalLabel->setStyleSheet("color:#e8a040;");
        } else {
            m_totalLabel->setText(main);
            m_totalLabel->setStyleSheet("");
        }
    } else {
        m_sliderMaxDs = 0;
        m_totalLabel->setText(tr("unknown (no STREAMINFO total)"));
    }
}

void MainWindow::recompute()
{
    m_plan = FcPlan{};
    m_outPath.clear();

    if (!m_probeOk) {
        m_outLabel->setText(QStringLiteral("--:--:--"));
        m_startSampLabel->setText(QStringLiteral("—"));
        m_lenSampLabel->setText(QStringLiteral("—"));
        m_outPathLabel->setText(QStringLiteral("—"));
        m_processBtn->setEnabled(false);
        return;
    }

    double startSec = 0.0, lenSec = 0.0;
    if (!parseHms(m_inEdit->text(), startSec)) {
        m_statusLabel->setText(tr("IN not in HH:MM:SS form."));
        m_processBtn->setEnabled(false);
        return;
    }
    if (!parseHms(m_lenEdit->text(), lenSec)) {
        m_statusLabel->setText(tr("Duration not in HH:MM:SS form."));
        m_processBtn->setEnabled(false);
        return;
    }

    fc_plan(startSec, lenSec, m_probe.msps, m_probe.msps_known,
            m_probe.header_sample_rate, m_probe.total_samples,
            m_probe.total_samples_known, &m_plan);

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

    m_outLabel->setText(secsToHms(startSec + lenSec));
    m_startSampLabel->setText(tr("%1  (@ %2 Hz)")
        .arg(ulongStr(m_plan.start_samples))
        .arg(m_plan.real_sample_rate_hz, 0, 'f', 0));
    m_lenSampLabel->setText(ulongStr(m_plan.length_samples));
    m_outPathLabel->setText(m_outPath);
    m_processBtn->setEnabled(true);
    m_statusLabel->setText(tr("Plan ready: %1 + %2 samples.")
        .arg(ulongStr(m_plan.start_samples), ulongStr(m_plan.length_samples)));

    // keep the slider mirrors of the edit fields (signals blocked)
    m_syncing = true;
    syncSliderFromEdits();
    m_syncing = false;
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
        QString err = QString::fromUtf8(r.stderr).trimmed();
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
