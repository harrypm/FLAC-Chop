#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QProgressBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QSignalBlocker>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QDesktopServices>
#include <QDateTime>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QVersionNumber>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <cmath>
#include "rangeslider.h"

// Git-derived build version (matches QCoreApplication::applicationVersion()).
#ifndef FLAC_CHOP_VERSION
#define FLAC_CHOP_VERSION "dev-unknown"
#endif

static QString ulongStr(quint64 v)
{
    // group with thousands separators for readability
    QString s = QString::number(v);
    int n = s.size();
    for (int i = n - 3; i > 0; i -= 3)
        s.insert(i, QLatin1Char(','));
    return s;
}

static QString rfFilterProfileText(quint64 outHeaderRateHz)
{
    switch (outHeaderRateHz) {
    case 16000: return QStringLiteral("sinc -n 2500 0-7650");
    case 20000: return QStringLiteral("sinc -n 2500 0-9650");
    case 24000: return QStringLiteral("sinc -n 2500 0-9400");
    case 28600: return QStringLiteral("sinc -n 2500 0-9400");
    default: return QString();
    }
}

static QString modeDisplay(quint64 outHeaderRateHz)
{
    switch (outHeaderRateHz) {
    case 16000: return QStringLiteral("16 MSPS");
    case 20000: return QStringLiteral("20 MSPS");
    case 24000: return QStringLiteral("24 MSPS");
    case 28600: return QStringLiteral("28.6 MSPS (8fsc)");
    default: return QStringLiteral("source rate");
    }
}

static QVersionNumber parseReleaseVersion(const QString& raw)
{
    QString s = raw.trimmed();
    if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        s.remove(0, 1);
    const int plus = s.indexOf(QLatin1Char('+'));
    if (plus > 0)
        s = s.left(plus);
    const int dash = s.indexOf(QLatin1Char('-'));
    if (dash > 0)
        s = s.left(dash);
    return QVersionNumber::fromString(s);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("FLAC-Chop %1 — RF capture cutter")
        .arg(QStringLiteral(FLAC_CHOP_VERSION)));
    resize(720, 580);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // --- Top menu bar ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* openAct = fileMenu->addAction(tr("&Open FLAC..."));
    connect(openAct, &QAction::triggered, this, &MainWindow::browse);
    fileMenu->addSeparator();
    QAction* exitAct = fileMenu->addAction(tr("E&xit"));
    connect(exitAct, &QAction::triggered, this, &QWidget::close);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* checkUpdatesAct = helpMenu->addAction(tr("Check for &Updates"));
    connect(checkUpdatesAct, &QAction::triggered, this, &MainWindow::checkForUpdatesManual);
    QAction* docsAct = helpMenu->addAction(tr("&Documentation (README)"));
    connect(docsAct, &QAction::triggered, this, [this]() {
        const QUrl docsUrl(QStringLiteral("https://github.com/harrypm/FLAC-Chop#readme"));
        if (!QDesktopServices::openUrl(docsUrl))
            m_statusLabel->setText(tr("Unable to open documentation URL."));
    });

    // --- Input file ---
    // Read-only QLineEdit (not a flat QLabel) so the input path sits in the
    // same sunken "in-lay" frame as the Output Directory field — both
    // top-row boxes then look identical, with clear white text on the dark
    // Fusion Base background.
    auto* inBox = new QGroupBox(tr("Input File"), central);
    auto* inLay = new QHBoxLayout(inBox);
    m_pathLabel = new QLineEdit(inBox);
    m_pathLabel->setReadOnly(true);
    m_pathLabel->setText(tr("(no file selected)"));
    m_pathLabel->setToolTip(tr("Full path of the loaded FLAC file."));
    m_browseBtn = new QPushButton(tr("Browse..."), inBox);
    inLay->addWidget(m_pathLabel, 1);
    inLay->addWidget(m_browseBtn, 0);

    // --- Output directory (top row, next to the input file) ---
    // Where cuts are written. Defaults to the input file's directory (the
    // original sibling -cut.flac behaviour); the user can Browse for a
    // dedicated output folder, persisted across sessions via QSettings.
    auto* outDirBox = new QGroupBox(tr("Output Directory"), central);
    auto* outDirLay = new QHBoxLayout(outDirBox);
    m_outDirEdit = new QLineEdit(outDirBox);
    m_outDirEdit->setPlaceholderText(tr("(same as input file)"));
    m_outDirEdit->setToolTip(tr("Cuts are written here. Empty = next to the input file."));
    m_outDirBrowseBtn = new QPushButton(tr("Browse..."), outDirBox);
    outDirLay->addWidget(m_outDirEdit, 1);
    outDirLay->addWidget(m_outDirBrowseBtn, 0);

    // Input + output side by side at the very top of the window.
    auto* ioRow = new QHBoxLayout();
    ioRow->addWidget(inBox, 1);
    ioRow->addWidget(outDirBox, 1);
    root->addLayout(ioRow);

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
    m_totalLabel->setWordWrap(true);
    m_totalLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLay->addRow(tr("Header rate:"), m_headerRateLabel);
    infoLay->addRow(tr("Bits / Channels:"), m_bitsChLabel);
    infoLay->addRow(tr("MSPS (from name):"), m_mspsLabel);
    infoLay->addRow(tr("Total (real):"), m_totalLabel);
    root->addWidget(infoBox);

    // --- Output processing ---
    auto* outBox = new QGroupBox(tr("Output Processing"), central);
    auto* outLay = new QFormLayout(outBox);
    m_outputModeCombo = new QComboBox(outBox);
    m_outputModeCombo->addItem(tr("Keep source rate"), quint64(0));
    m_outputModeCombo->setEnabled(false);
    m_outputBitsCombo = new QComboBox(outBox);
    m_outputBitsCombo->addItem(tr("Keep source bit-depth"), uint(0));
    m_outputBitsCombo->addItem(tr("8-bit"), uint(8));
    m_outputBitsCombo->addItem(tr("6-bit crush (stored as 8-bit FLAC)"), uint(6));
    m_outputBitsCombo->setEnabled(false);
    m_basicFilterCheck = new QCheckBox(tr("Apply basic RF filter profile"), outBox);
    m_basicFilterCheck->setChecked(true);
    m_basicFilterCheck->setEnabled(false);
    m_filterProfileLabel = new QLabel(QStringLiteral("—"), outBox);
    m_filterProfileLabel->setWordWrap(true);
    m_filterProfileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outLay->addRow(tr("Output mode:"), m_outputModeCombo);
    outLay->addRow(tr("Bit-depth:"), m_outputBitsCombo);
    outLay->addRow(QString(), m_basicFilterCheck);
    outLay->addRow(tr("Filter profile:"), m_filterProfileLabel);
    root->addWidget(outBox);

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
    m_processBtn = new QPushButton(tr("Process FLAC"), central);
    m_processBtn->setEnabled(false);
    m_cancelBtn = new QPushButton(tr("Cancel"), central);
    m_cancelBtn->setEnabled(false); // only live while a cut is in flight
    m_cancelBtn->setToolTip(tr("Stop the in-progress cut."));
    auto* actionLay = new QHBoxLayout();
    actionLay->addWidget(m_processBtn, 1);
    actionLay->addWidget(m_cancelBtn, 0);
    // 'Check for Updates' lives only in the Help menu now (auto-checked on
    // startup); no button next to Process to keep the action row clean.
    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_statusLabel = new QLabel(tr("Ready — select a FLAC file."), central);
    m_statusLabel->setWordWrap(true);
    root->addLayout(actionLay);
    root->addWidget(m_progress);
    root->addWidget(m_statusLabel);
    root->addStretch(1);

    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::browse);
    connect(m_processBtn, &QPushButton::clicked, this, &MainWindow::process);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::cancelProcess);
    connect(m_setInBtn, &QPushButton::clicked, this, &MainWindow::setInFromBox);
    connect(m_setOutBtn, &QPushButton::clicked, this, &MainWindow::setOutFromBox);
    connect(m_slider, &QRangeSlider::inValueChanged, this, &MainWindow::onSliderInChanged);
    connect(m_slider, &QRangeSlider::outValueChanged, this, &MainWindow::onSliderOutChanged);
    connect(m_outputModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyCut);
    connect(m_outputBitsCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyCut);
    connect(m_basicFilterCheck, &QCheckBox::toggled,
            this, &MainWindow::applyCut);
    connect(m_outDirBrowseBtn, &QPushButton::clicked, this, &MainWindow::browseOutDir);
    connect(m_outDirEdit, &QLineEdit::editingFinished, this, &MainWindow::onOutDirEdited);

    // Restore the persisted output directory (empty = "same as input").
    // If a dir was previously chosen (non-empty), auto-follow is off so it
    // persists; if empty, auto-follow stays on and the field will track the
    // input file's dir on first load.
    {
        QSettings s;
        s.beginGroup(QStringLiteral("output"));
        const QString persisted = s.value(QStringLiteral("dir")).toString().trimmed();
        s.endGroup();
        m_outDirEdit->setText(persisted);
        m_outDirAutoFollow = persisted.isEmpty();
    }

    // Drag & drop: the window accepts file drops; the time box must not swallow them.
    setAcceptDrops(true);
    m_timeEdit->setAcceptDrops(false);

    m_watcher = new QFutureWatcher<FcChopResult>(this);
    connect(m_watcher, &QFutureWatcher<FcChopResult>::finished,
            this, &MainWindow::onChopFinished);

    m_probeWatcher = new QFutureWatcher<FcProbe>(this);
    connect(m_probeWatcher, &QFutureWatcher<FcProbe>::finished,
            this, &MainWindow::onProbeFinished);
    m_net = new QNetworkAccessManager(this);

    if (!fc_sox_available()) {
        m_statusLabel->setText(tr("WARNING: SoX not found (bundled or PATH) — cutting will fail."));
    }
    maybeCheckForUpdates();
}

void MainWindow::setControlsEnabled(bool enabled)
{
    m_browseBtn->setEnabled(enabled);
    m_processBtn->setEnabled(enabled && m_probeOk && m_plan.ok);
    m_timeEdit->setEnabled(enabled);
    m_setInBtn->setEnabled(enabled && m_probeOk);
    m_setOutBtn->setEnabled(enabled && m_probeOk);
    m_outputModeCombo->setEnabled(enabled && m_probeOk && m_outputModeCombo->count() > 1);
    m_outputBitsCombo->setEnabled(enabled && m_probeOk);
    const quint64 outRate = m_outputModeCombo->currentData().toULongLong();
    m_basicFilterCheck->setEnabled(enabled && m_probeOk && outRate > 0);
    m_outDirEdit->setEnabled(enabled);
    m_outDirBrowseBtn->setEnabled(enabled);
}

void MainWindow::browse()
{
    if (m_probing)
        return;
    const QString startDir = m_inPath.isEmpty() ? QDir::homePath() : QFileInfo(m_inPath).absolutePath();
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Select capture file"), startDir,
        tr("RF captures (*.flac *.ldf *.wav *.u8 *.u16 *.s8 *.s16 *.r8 *.r16 *.raw *.bin)"
           ";;FLAC files (*.flac);;All files (*)"));
    if (fn.isEmpty())
        return;
    loadFile(fn);
}

QString MainWindow::effectiveOutDir() const
{
    // User-chosen dir wins; empty (or a dir that doesn't yet exist but the
    // user typed) falls back to the input file's directory so cuts always land
    // somewhere sensible by default. Returns "" when no file is loaded.
    const QString chosen = m_outDirEdit ? m_outDirEdit->text().trimmed() : QString();
    if (!chosen.isEmpty())
        return chosen;
    if (m_inPath.isEmpty())
        return QString();
    return QFileInfo(m_inPath).absolutePath();
}

void MainWindow::persistOutDir(const QString& dir)
{
    QSettings s;
    s.beginGroup(QStringLiteral("output"));
    s.setValue(QStringLiteral("dir"), dir);
    s.endGroup();
}

void MainWindow::browseOutDir()
{
    const QString startDir = effectiveOutDir().isEmpty() ? QDir::homePath() : effectiveOutDir();
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select output directory"), startDir);
    if (d.isEmpty())
        return;
    m_outDirEdit->setText(d);
    m_outDirAutoFollow = false; // user explicitly chose a dir — stop auto-following
    persistOutDir(d);
    applyCut();
}

void MainWindow::onOutDirEdited()
{
    // editingFinished fires on focus loss / Enter — persist + recompute the
    // output path preview. Empty text means "same as input" (stock default).
    const QString d = m_outDirEdit->text().trimmed();
    // If the user clears the field, resume auto-following the input dir.
    m_outDirAutoFollow = d.isEmpty();
    persistOutDir(d);
    applyCut();
}

QString MainWindow::renamedOutputStem() const
{
    // Rename the output stem to reflect the new altered metadata when the
    // input name matches the MISRC capture naming convention, which (per
    // MISRC-GUI gui_settings.c) is:  <base>_<rfTag>_<B>-bit_<N>msps[.flac]
    // i.e. bits first, then rate — e.g.  ..._8-bit_20msps  ->  ..._6-bit_16msps.
    // "Keep source rate/bits" keeps the original token. Non-matching names
    // return "" (stock <stem>-cut).
    if (m_inPath.isEmpty())
        return QString();
    const QString inStem = QFileInfo(m_inPath).completeBaseName();
    // Match an optional prefix, then <B>-bit_<N>msps, then an optional suffix.
    static const QRegularExpression re(QStringLiteral("^(.*?)([0-9]+)-bit_([0-9]+)msps(.*)$"));
    const auto m = re.match(inStem);
    if (!m.hasMatch())
        return QString();
    const QString prefix = m.captured(1);
    const QString srcBitsTok = m.captured(2);
    const QString srcMspsTok = m.captured(3);
    const QString suffix = m.captured(4);

    // New bits token: the selected output bit-depth, or keep the source token
    // when "keep source bit-depth".
    const uint outBits = m_outputBitsCombo ? m_outputBitsCombo->currentData().toUInt() : 0;
    QString bitsTok = srcBitsTok;
    if (outBits > 0)
        bitsTok = QString::number(outBits);

    // New rate token: the selected output mode (header kHz / 1000 = MSPS), or
    // keep the source token when "keep source rate".
    const quint64 outHeaderHz = m_outputModeCombo ? m_outputModeCombo->currentData().toULongLong() : 0;
    QString mspsTok = srcMspsTok;
    if (outHeaderHz > 0)
        mspsTok = QString::number(outHeaderHz / 1000);

    return prefix + bitsTok + QStringLiteral("-bit_") + mspsTok + QStringLiteral("msps") + suffix;
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
    {
        QSignalBlocker b1(m_outputModeCombo);
        m_outputModeCombo->clear();
        m_outputModeCombo->addItem(tr("Keep source rate"), quint64(0));
        m_outputModeCombo->setEnabled(false);
    }
    {
        QSignalBlocker b2(m_outputBitsCombo);
        m_outputBitsCombo->setCurrentIndex(0);
        m_outputBitsCombo->setEnabled(false);
    }
    {
        QSignalBlocker b3(m_basicFilterCheck);
        m_basicFilterCheck->setChecked(true);
        m_basicFilterCheck->setEnabled(false);
    }
    m_filterProfileLabel->setText(QStringLiteral("—"));

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
    m_pathLabel->setToolTip(fn);

    // Output dir: if auto-following (no explicit user choice yet), update the
    // field to the NEW input's directory so cuts land next to the new file. If
    // the user chose a dedicated dir, it persists and is left untouched.
    if (m_outDirAutoFollow)
        m_outDirEdit->setText(QFileInfo(fn).absolutePath());

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
    {
        QSignalBlocker b(m_outputModeCombo);
        m_outputModeCombo->clear();
        m_outputModeCombo->addItem(tr("Keep source rate"), quint64(0));
        if (m_probe.is_rf) {
            struct ModeEntry { quint64 headerRateHz; const char* label; };
            const ModeEntry modes[] = {
                {20000, "20 MSPS"},
                {24000, "24 MSPS"},
                {28600, "28.6 MSPS (8fsc)"},
                {16000, "16 MSPS (VHS experimental)"},
            };
            for (const auto& m : modes) {
                const double outRealHz = double(m.headerRateHz) * 1000.0;
                if (m_probe.real_rate_hz + 0.5 >= outRealHz)
                    m_outputModeCombo->addItem(tr(m.label), m.headerRateHz);
            }
        }
        m_outputModeCombo->setCurrentIndex(0);
    }
    {
        QSignalBlocker b(m_outputBitsCombo);
        m_outputBitsCombo->setCurrentIndex(0);
    }
    {
        QSignalBlocker b(m_basicFilterCheck);
        m_basicFilterCheck->setChecked(true);
    }

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
    {
        // Echo non-fatal probe warnings in the status line too, so they're
        // visible without hovering the total label. Cleared on the next action.
        const QString warnings = QString::fromUtf8(m_probe.warnings).trimmed();
        if (!warnings.isEmpty())
            m_statusLabel->setText(tr("Probed OK (⚠ warnings): %1 — type a time + Set IN/OUT, then Process.").arg(warnings));
        else
            m_statusLabel->setText(tr("Probed OK. Type a time + Set IN/OUT, or drag the slider, then Process."));
    }
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
    // Accept anything that looks like a capture: the core probe sniffs FLAC
    // / WAV by magic header (so unknown extensions with the right magic
    // work) and maps the raw PCM extensions. Anything else fails the probe
    // with a clear error in the status line.
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
        m_totalLabel->setToolTip(QString());
        m_totalLabel->setStyleSheet(QString());
        return;
    }
    if (m_probe.format >= 2) {
        // Headerless raw PCM: there is no header to report; the rate label
        // points at the filename (the <n>msps hint is the only rate source).
        static const char* kRawNames[] = { "", "", "raw u8", "raw s8", "raw u16", "raw s16" };
        const QString fmtName = (m_probe.format <= 5)
            ? QString::fromLatin1(kRawNames[m_probe.format])
            : QStringLiteral("?");
        m_headerRateLabel->setText(tr("raw PCM (%1, rate from filename)").arg(fmtName));
    } else {
        m_headerRateLabel->setText(tr("%1 Hz (header)").arg(m_probe.header_sample_rate));
    }
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

    // Surface non-fatal probe diagnostics (tag-unit corrections, scan
    // misalignment, vorbis self-consistency mismatches). Append a ⚠ marker
    // to the total label, put the full text in the tooltip, and tint it so
    // it's noticed. Empty when everything checked out.
    const QString warnings = QString::fromUtf8(m_probe.warnings).trimmed();
    if (!warnings.isEmpty()) {
        m_totalLabel->setText(m_totalLabel->text() + QStringLiteral("  ⚠ ") + warnings);
        m_totalLabel->setToolTip(tr("Probe warnings:\n%1").arg(warnings));
        m_totalLabel->setStyleSheet("color:#e8a040;");
    } else {
        m_totalLabel->setToolTip(QString());
    }
}

void MainWindow::applyCut()
{
    m_plan = FcPlan{};
    m_outPath.clear();
    const quint64 outHeaderRateHz = m_outputModeCombo->currentData().toULongLong();
    const uint outBits = m_outputBitsCombo->currentData().toUInt();
    const bool useBasicFilter = (outHeaderRateHz > 0) && m_basicFilterCheck->isChecked();

    if (!m_probeOk) {
        m_inLabel->setText(QStringLiteral("--:--:--.--"));
        m_outLabel->setText(QStringLiteral("--:--:--.--"));
        m_durLabel->setText(QStringLiteral("--:--:--.--"));
        m_startSampLabel->setText(QStringLiteral("—"));
        m_lenSampLabel->setText(QStringLiteral("—"));
        m_outPathLabel->setText(QStringLiteral("—"));
        m_filterProfileLabel->setText(QStringLiteral("—"));
        m_processBtn->setEnabled(false);
        return;
    }
    if (outHeaderRateHz == 0) {
        m_filterProfileLabel->setText(tr("off (keeping source rate)"));
    } else if (useBasicFilter) {
        const QString profile = rfFilterProfileText(outHeaderRateHz);
        if (profile.isEmpty())
            m_filterProfileLabel->setText(tr("on (no preset profile for this rate)"));
        else
            m_filterProfileLabel->setText(profile);
    } else {
        m_filterProfileLabel->setText(tr("off"));
    }
    m_basicFilterCheck->setEnabled(m_probeOk && outHeaderRateHz > 0 && m_browseBtn->isEnabled());

    if (outHeaderRateHz > 0) {
        if (!m_probe.is_rf) {
            m_statusLabel->setText(tr("Output MSPS modes are only available for RF captures."));
            m_processBtn->setEnabled(false);
            return;
        }
        const double outRealHz = double(outHeaderRateHz) * 1000.0;
        if (outRealHz > m_probe.real_rate_hz + 0.5) {
            m_statusLabel->setText(tr("Selected output mode (%1) is above input rate.")
                .arg(modeDisplay(outHeaderRateHz)));
            m_processBtn->setEnabled(false);
            return;
        }
    }

    double startSec = m_inSec;
    double lenSec = m_outSec - m_inSec;
    if (lenSec <= 0.0) {
        m_statusLabel->setText(tr("OUT must be after IN."));
        m_processBtn->setEnabled(false);
        return;
    }

    // fc_plan computes sample counts for SoX's trim command. The `s` suffix
    // means sample counts, and SoX reads the file at its native frame rate —
    // each FLAC frame is one real RF sample. For /1000 RF captures the STREAMINFO
    // header rate is the /1000 "kHz" value (20000) but each sample IS a real
    // 20 MHz sample, so the real rate (20M) must be used to compute the sample
    // count for a given duration. The STREAMINFO total_samples (208M) limits
    // how many SoX will read (≈10.4 s at 20 MHz); fc_plan clamps to the real
    // total_samples (208B) so requests beyond the file are handled gracefully.
    fc_plan(startSec, lenSec, m_probe.real_rate_hz,
            m_probe.total_samples, m_probe.total_samples_known, &m_plan);

    if (!m_plan.ok) {
        m_statusLabel->setText(tr("Plan error: %1")
            .arg(QString::fromUtf8(m_plan.error)));
        m_processBtn->setEnabled(false);
        return;
    }

    // Output path via the Rust helper — into the effective output directory,
    // with a renamed stem that reflects the new altered metadata
    // (e.g. 20msps_8-bit -> 16msps_6-bit) when the input name matches the
    // MISRC capture naming convention.
    char buf[4096];
    const QString outDir = effectiveOutDir();
    const QByteArray outDirB = outDir.toUtf8();
    const QString stem = renamedOutputStem();
    const QByteArray stemB = stem.toUtf8();
    if (fc_generate_output_path(m_inPath.toUtf8().constData(),
                                 outDirB.constData(),
                                 stemB.constData(), buf, sizeof(buf))) {
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
    const QString bitsText =
        (outBits == 0) ? tr("source depth")
      : (outBits == 6) ? tr("6-bit crush in 8-bit FLAC")
      : tr("%1-bit").arg(outBits);
    const QString modeText = modeDisplay(outHeaderRateHz);
    m_statusLabel->setText(tr("Plan ready: %1 + %2 samples | output %3 | %4.")
        .arg(ulongStr(m_plan.start_samples),
             ulongStr(m_plan.length_samples),
             modeText,
             bitsText));
}

void MainWindow::process()
{
    if (!m_probeOk || !m_plan.ok || m_outPath.isEmpty())
        return;
    const quint64 outHeaderRateHz = m_outputModeCombo->currentData().toULongLong();
    const quint32 outBits = m_outputBitsCombo->currentData().toUInt();
    const qint32 basicFilter = (outHeaderRateHz > 0 && m_basicFilterCheck->isChecked()) ? 1 : 0;

    // Ensure the chosen output directory exists before SoX writes into it.
    const QString outDir = effectiveOutDir();
    if (!outDir.isEmpty() && !QDir().exists(outDir)) {
        if (!QDir().mkpath(outDir)) {
            m_statusLabel->setText(tr("Cannot create output directory: %1").arg(outDir));
            return;
        }
    }

    if (QFile::exists(m_outPath)) {
        auto r = QMessageBox::question(this, tr("Overwrite?"),
            tr("Output file exists:\n%1\nOverwrite?").arg(m_outPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes)
            return;
    }

    setControlsEnabled(false);
    m_progress->setRange(0, 0); // busy indicator
    m_cancelRequested = false;
    m_cancelBtn->setEnabled(true);
    const QString bitsText =
        (outBits == 0) ? tr("source depth")
      : (outBits == 6) ? tr("6-bit crush")
      : tr("%1-bit").arg(outBits);
    m_statusLabel->setText(tr("Processing... trim %1s %2s | %3 | %4")
        .arg(ulongStr(m_plan.start_samples),
             ulongStr(m_plan.length_samples),
             modeDisplay(outHeaderRateHz),
             bitsText));

    const QString inPath = m_inPath;
    const QString outPath = m_outPath;
    const quint64 start = m_plan.start_samples;
    const quint64 len = m_plan.length_samples;
    const qint32 isRf = m_probe.is_rf ? 1 : 0;
    auto fut = QtConcurrent::run([inPath, outPath, start, len, outHeaderRateHz, outBits, basicFilter, isRf]() -> FcChopResult {
        FcChopResult r{};
        QByteArray inB = inPath.toUtf8();
        QByteArray outB = outPath.toUtf8();
        fc_chop(inB.constData(), outB.constData(), start, len,
                outHeaderRateHz, outBits, basicFilter, isRf, &r);
        return r;
    });
    m_watcher->setFuture(fut);
}

void MainWindow::cancelProcess()
{
    if (!m_watcher || !m_watcher->isRunning())
        return;
    m_cancelRequested = true;
    fc_chop_cancel(); // tell the Rust core to kill the sox child
    m_cancelBtn->setEnabled(false); // prevent repeat clicks
    m_statusLabel->setText(tr("Cancelling…"));
}

void MainWindow::onChopFinished()
{
    FcChopResult r = m_watcher->result();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    m_cancelBtn->setEnabled(false);

    if (m_cancelRequested) {
        // The sox child was killed mid-cut: remove the partial output file so
        // the next run's clobber-avoidance doesn't see a corrupt -cut.flac.
        m_cancelRequested = false;
        if (!m_outPath.isEmpty() && QFile::exists(m_outPath))
            QFile::remove(m_outPath);
        m_statusLabel->setText(tr("Cancelled."));
    } else if (r.ok) {
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

void MainWindow::checkForUpdatesManual()
{
    checkForUpdates(true);
}

void MainWindow::maybeCheckForUpdates()
{
    static constexpr qint64 kSevenDaysSeconds = 7ll * 24ll * 60ll * 60ll;
    QSettings settings;
    settings.beginGroup(QStringLiteral("updates"));
    const qint64 lastCheckUtc = settings.value(QStringLiteral("last_check_utc"), 0).toLongLong();
    settings.endGroup();
    const qint64 nowUtc = QDateTime::currentSecsSinceEpoch();
    if (lastCheckUtc > 0 && nowUtc > lastCheckUtc && (nowUtc - lastCheckUtc) < kSevenDaysSeconds)
        return;
    checkForUpdates(false);
}

void MainWindow::checkForUpdates(bool manual)
{
    if (!m_net) {
        if (manual)
            m_statusLabel->setText(tr("Update check unavailable: network manager not initialized."));
        return;
    }
    if (m_updateCheckInFlight) {
        if (manual)
            m_statusLabel->setText(tr("Update check already in progress."));
        return;
    }

    m_updateCheckInFlight = true;

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/harrypm/FLAC-Chop/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("FLAC-Chop/%1").arg(QCoreApplication::applicationVersion()));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual]() {
        auto finish = [this, reply]() {
            m_updateCheckInFlight = false;
            reply->deleteLater();
        };

        if (reply->error() != QNetworkReply::NoError) {
            if (manual)
                m_statusLabel->setText(tr("Update check failed: %1").arg(reply->errorString()));
            finish();
            return;
        }

        const QByteArray body = reply->readAll();
        QJsonParseError parseErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            if (manual)
                m_statusLabel->setText(tr("Update check failed: invalid release metadata."));
            finish();
            return;
        }

        const QJsonObject obj = doc.object();
        const QString tagName = obj.value(QStringLiteral("tag_name")).toString().trimmed();
        const QString releaseName = obj.value(QStringLiteral("name")).toString().trimmed();
        const QString releaseUrl = obj.value(QStringLiteral("html_url")).toString().trimmed();
        QString latestDisplay = tagName.isEmpty() ? releaseName : tagName;
        if (latestDisplay.isEmpty())
            latestDisplay = tr("(unknown)");
        const QString appVersion = QCoreApplication::applicationVersion().trimmed();
        const QString currentDisplay = appVersion.isEmpty() ? tr("(unknown)") : appVersion;

        // Record successful checks to enforce the 7-day automatic cadence.
        QSettings settings;
        settings.beginGroup(QStringLiteral("updates"));
        settings.setValue(QStringLiteral("last_check_utc"), QDateTime::currentSecsSinceEpoch());
        settings.endGroup();

        const QVersionNumber latestVersion = parseReleaseVersion(latestDisplay);
        const QVersionNumber currentVersion = parseReleaseVersion(appVersion);
        const bool comparable = !latestVersion.isNull() && !currentVersion.isNull();
        const bool updateAvailable = comparable && (QVersionNumber::compare(latestVersion, currentVersion) > 0);

        if (updateAvailable) {
            const QString msg = tr("Update available: %1 (current %2).")
                .arg(latestDisplay, currentDisplay);
            m_statusLabel->setText(msg);
            if (manual) {
                QMessageBox box(this);
                box.setIcon(QMessageBox::Information);
                box.setWindowTitle(tr("Update available"));
                box.setText(msg);
                QPushButton* openBtn = nullptr;
                if (!releaseUrl.isEmpty()) {
                    box.setInformativeText(releaseUrl);
                    openBtn = box.addButton(tr("Open release page"), QMessageBox::AcceptRole);
                    box.addButton(QMessageBox::Close);
                } else {
                    box.addButton(QMessageBox::Ok);
                }
                box.exec();
                if (openBtn && box.clickedButton() == openBtn)
                    QDesktopServices::openUrl(QUrl(releaseUrl));
            }
            finish();
            return;
        }

        if (manual) {
            if (comparable) {
                const QString msg = tr("No update found. Current version: %1.").arg(currentDisplay);
                m_statusLabel->setText(msg);
                QMessageBox::information(this, tr("Up to date"), msg);
            } else {
                const QString msg = tr("Latest release seen: %1 (unable to compare to current version %2).")
                    .arg(latestDisplay, currentDisplay);
                m_statusLabel->setText(msg);
                QMessageBox box(this);
                box.setIcon(QMessageBox::Information);
                box.setWindowTitle(tr("Update check"));
                box.setText(msg);
                QPushButton* openBtn = nullptr;
                if (!releaseUrl.isEmpty()) {
                    box.setInformativeText(releaseUrl);
                    openBtn = box.addButton(tr("Open release page"), QMessageBox::AcceptRole);
                    box.addButton(QMessageBox::Close);
                } else {
                    box.addButton(QMessageBox::Ok);
                }
                box.exec();
                if (openBtn && box.clickedButton() == openBtn)
                    QDesktopServices::openUrl(QUrl(releaseUrl));
            }
        }

        finish();
    });
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
