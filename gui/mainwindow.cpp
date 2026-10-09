#include "mainwindow.h"

#include "batchtab.h"
#include "syncedittab.h"
#include "stemutil.h"
#include "theme.h"

#include <QApplication>
#include <QRadioButton>
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
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QSignalBlocker>
#include <QTimer>
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
    case 10000: return QStringLiteral("sinc -n 2500 0-3050");
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
    case 10000: return QStringLiteral("10 MSPS (HiFi FM)");
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
    // The outer layout just hosts the tab bar with no margins so the tabs sit
    // flush at the top; each page carries its own 12 px margin.
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tabs = new QTabWidget(central);
    root->addWidget(m_tabs);

    // --- Chop page (the existing cutter UI) ---
    auto* chopPage = new QWidget(m_tabs);
    auto* chopLay = new QVBoxLayout(chopPage);
    chopLay->setContentsMargins(12, 12, 12, 12);
    chopLay->setSpacing(10);

    // --- Top menu bar ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* openAct = fileMenu->addAction(tr("&Open Data..."));
    connect(openAct, &QAction::triggered, this, &MainWindow::browse);
    fileMenu->addSeparator();
    QAction* exitAct = fileMenu->addAction(tr("E&xit"));
    connect(exitAct, &QAction::triggered, this, &QWidget::close);

    // --- Theme menu (top bar, a peer of File/Help): just Dark / Light ---
    // The tbc-tools Lite/Dark modes. Applied immediately + remembered
    // (QSettings "theme/mode"); with no saved choice the default follows the
    // OS theme (the tbc-tools behaviour).
    auto* themeMenu = menuBar()->addMenu(tr("&Theme"));
    QAction* themeDarkAct = themeMenu->addAction(tr("&Dark"));
    QAction* themeLightAct = themeMenu->addAction(tr("&Light"));
    themeDarkAct->setCheckable(true);
    themeLightAct->setCheckable(true);
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    themeGroup->addAction(themeDarkAct);
    themeGroup->addAction(themeLightAct);
    {
        // Reflect the ACTIVE theme (the same resolution as startup: an
        // explicit saved choice, else Dark — the default).
        QSettings ts;
        ts.beginGroup(QStringLiteral("theme"));
        const QString mode = ts.value(QStringLiteral("mode")).toString().toLower();
        ts.endGroup();
        const bool dark =
            (mode == QStringLiteral("dark") || mode == QStringLiteral("light"))
                ? (mode == QStringLiteral("dark"))
                : true; // no saved choice: Dark is the default
        if (dark)
            themeDarkAct->setChecked(true);
        else
            themeLightAct->setChecked(true);
    }
    connect(themeDarkAct, &QAction::triggered, this, [this] { applyThemeChoice(true); });
    connect(themeLightAct, &QAction::triggered, this, [this] { applyThemeChoice(false); });

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
    // same sunken "in-lay" frame as the Output File field — both
    // top-row boxes then look identical, with clear white text on the dark
    // Fusion Base background.
    auto* inBox = new QGroupBox(tr("Input File"), chopPage);
    auto* inLay = new QHBoxLayout(inBox);
    m_pathLabel = new QLineEdit(inBox);
    m_pathLabel->setReadOnly(true);
    m_pathLabel->setText(tr("(no file selected)"));
    m_pathLabel->setToolTip(tr("Full path of the loaded FLAC file."));
    m_browseBtn = new QPushButton(tr("Browse..."), inBox);
    inLay->addWidget(m_pathLabel, 1);
    inLay->addWidget(m_browseBtn, 0);

    // --- Output file (top row, next to the input file) ---
    // The destination of the cut as a full path + name: the auto-derived
    // path (next to the input file) until edited — rename in place, or type
    // any full path; a bare name lands in the input file's folder as
    // <name>.flac. Clearing the box restores the auto path. (This REPLACED
    // the old Output Directory box: a dedicated folder is just a full path
    // typed here with the name left editable — one destination field, at
    // the top where the input is.)
    auto* outFileBox = new QGroupBox(tr("Output File"), chopPage);
    auto* outFileLay = new QHBoxLayout(outFileBox);
    m_outPathEdit = new QLineEdit(outFileBox);
    m_outPathEdit->setToolTip(tr("The output file path + name (the auto-derived path until you edit it). Rename by editing the text; a bare name lands in the input file's folder as <name>.flac; clear the box to restore the auto path. Edits while a cut runs apply to the NEXT cut."));
    m_outPathEdit->setEnabled(false); // enabled once a file is probed (setControlsEnabled)
    m_outPathBrowseBtn = new QPushButton(tr("Browse..."), outFileBox);
    m_outPathBrowseBtn->setToolTip(tr("Pick the output folder for the current file name (the full path in the box moves there)."));
    m_outPathBrowseBtn->setEnabled(false);
    outFileLay->addWidget(m_outPathEdit, 1);
    outFileLay->addWidget(m_outPathBrowseBtn, 0);

    // Input + output side by side at the very top of the window.
    auto* ioRow = new QHBoxLayout();
    ioRow->addWidget(inBox, 1);
    ioRow->addWidget(outFileBox, 1);
    chopLay->addLayout(ioRow);

    // --- Markers: one editable time box + Set IN / Set OUT buttons ---
    // Type a time, click Set IN or Set OUT to drop that marker. The cut is
    // only ever changed by an explicit action (button or slider drag), never
    // by typing alone, so there is no textChanged -> recompute feedback loop.
    auto* markerBox = new QGroupBox(tr("Markers (real time, HH:MM:SS)"), chopPage);
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
    chopLay->addWidget(markerBox);

    // --- Navigate: IN/OUT range slider (0.1 s resolution) ---
    auto* navBox = new QGroupBox(tr("Navigate — drag IN (green) / OUT (red) (0.1 s)"), chopPage);
    auto* navLay = new QVBoxLayout(navBox);
    m_slider = new QRangeSlider(navBox);
    m_slider->setEnabled(false);
    navLay->addWidget(m_slider);
    chopLay->addWidget(navBox);

    // --- Source info ---
    auto* infoBox = new QGroupBox(tr("Source Info (from FLAC STREAMINFO + filename)"), chopPage);
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
    chopLay->addWidget(infoBox);

    // --- Output processing ---
    auto* outBox = new QGroupBox(tr("Output Processing"), chopPage);
    auto* outLay = new QFormLayout(outBox);
    m_outputModeCombo = new QComboBox(outBox);
    m_outputModeCombo->addItem(tr("Keep source rate"), quint64(0));
    m_outputModeCombo->setEnabled(false);
    m_outputBitsCombo = new QComboBox(outBox);
    m_outputBitsCombo->addItem(tr("Keep source bit-depth"), uint(0));
    m_outputBitsCombo->addItem(tr("8-bit"), uint(8));
    m_outputBitsCombo->addItem(tr("12-bit (MISRC true 12-bit FLAC)"), uint(12));
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
    chopLay->addWidget(outBox);

    // --- Preview ---
    auto* prevBox = new QGroupBox(tr("Preview"), chopPage);
    auto* prevLay = new QFormLayout(prevBox);
    m_startSampLabel = new QLabel(QStringLiteral("—"), prevBox);
    m_lenSampLabel = new QLabel(QStringLiteral("—"), prevBox);
    prevLay->addRow(tr("Start sample:"), m_startSampLabel);
    prevLay->addRow(tr("Length samples:"), m_lenSampLabel);
    chopLay->addWidget(prevBox);

    // --- Process + progress + status ---
    m_processBtn = new QPushButton(tr("Process FLAC"), chopPage);
    m_processBtn->setEnabled(false);
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(200);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        const uint32_t pct = fc_chop_get_progress();
        if (pct <= 100) {
            m_progress->setValue(int(pct));
            m_progress->setFormat(tr("%p%"));
        }
    });
    m_cancelBtn = new QPushButton(tr("Cancel"), chopPage);
    m_cancelBtn->setEnabled(false); // only live while a cut is in flight
    m_cancelBtn->setToolTip(tr("Stop the in-progress cut."));
    auto* actionLay = new QHBoxLayout();
    actionLay->addWidget(m_processBtn, 1);
    actionLay->addWidget(m_cancelBtn, 0);
    // 'Check for Updates' lives only in the Help menu now (auto-checked on
    // startup); no button next to Process to keep the action row clean.
    m_progress = new QProgressBar(chopPage);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->setFormat(tr("%p%"));
    m_statusLabel = new QLabel(tr("Ready — select a FLAC file."), chopPage);
    m_statusLabel->setWordWrap(true);
    chopLay->addLayout(actionLay);
    chopLay->addWidget(m_progress);
    chopLay->addWidget(m_statusLabel);
    chopLay->addStretch(1);

    m_tabs->addTab(chopPage, tr("Chop"));

    // --- Batch Task page (multi-file queue + parallel processing) ---
    m_batchTab = new BatchTab(m_tabs);
    m_tabs->addTab(m_batchTab, tr("Batch Task"));

    // --- Sync Edit page (synchronized time-range cuts across a file set) ---
    m_syncEditTab = new SyncEditTab(m_tabs);
    m_tabs->addTab(m_syncEditTab, tr("Sync Edit"));

    // --- Metadata Editor page ---
    // Read-only STREAMINFO summary + an editable Vorbis-comment table that
    // writes back to the source FLAC in place via fc_replace_comments. The
    // write runs off-thread (it may splice a temp copy for large growth).
    auto* metaPage = new QWidget(m_tabs);
    auto* metaLay = new QVBoxLayout(metaPage);
    metaLay->setContentsMargins(12, 12, 12, 12);
    metaLay->setSpacing(10);

    auto* siBox = new QGroupBox(tr("Stream Info (read-only)"), metaPage);
    auto* siLay = new QFormLayout(siBox);
    m_metaFormatLabel = new QLabel(QStringLiteral("—"), siBox);
    m_metaHeaderRateLabel = new QLabel(QStringLiteral("—"), siBox);
    m_metaBitsChLabel = new QLabel(QStringLiteral("—"), siBox);
    m_metaRealRateLabel = new QLabel(QStringLiteral("—"), siBox);
    m_metaTotalSamplesLabel = new QLabel(QStringLiteral("—"), siBox);
    m_metaFileSizeLabel = new QLabel(QStringLiteral("—"), siBox);
    for (auto* lbl : {m_metaFormatLabel, m_metaHeaderRateLabel, m_metaBitsChLabel,
                      m_metaRealRateLabel, m_metaTotalSamplesLabel, m_metaFileSizeLabel}) {
        lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    siLay->addRow(tr("Format:"), m_metaFormatLabel);
    siLay->addRow(tr("Header rate:"), m_metaHeaderRateLabel);
    siLay->addRow(tr("Bits / Channels:"), m_metaBitsChLabel);
    siLay->addRow(tr("Real rate:"), m_metaRealRateLabel);
    siLay->addRow(tr("Total samples:"), m_metaTotalSamplesLabel);
    siLay->addRow(tr("File size:"), m_metaFileSizeLabel);
    metaLay->addWidget(siBox);

    auto* vcBox = new QGroupBox(tr("Vorbis Comments (editable)"), metaPage);
    auto* vcLay = new QVBoxLayout(vcBox);
    m_metaTable = new QTableWidget(0, 2, vcBox);
    m_metaTable->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    m_metaTable->verticalHeader()->setVisible(false);
    m_metaTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_metaTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_metaTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_metaTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_metaTable->setToolTip(tr("Edit any field/value, or add/remove rows. Field names must be A-Z a-z 0-9 _ (saved upper-case)."));
    vcLay->addWidget(m_metaTable);

    auto* metaEditRow = new QHBoxLayout();
    m_metaAddBtn = new QPushButton(tr("Add"), vcBox);
    m_metaRemoveBtn = new QPushButton(tr("Remove"), vcBox);
    m_metaUpBtn = new QPushButton(tr("Move Up"), vcBox);
    m_metaDownBtn = new QPushButton(tr("Move Down"), vcBox);
    m_metaAddBtn->setToolTip(tr("Append a new blank row."));
    m_metaRemoveBtn->setToolTip(tr("Remove the selected row."));
    m_metaUpBtn->setToolTip(tr("Move the selected row up."));
    m_metaDownBtn->setToolTip(tr("Move the selected row down."));
    metaEditRow->addWidget(m_metaAddBtn);
    metaEditRow->addWidget(m_metaRemoveBtn);
    metaEditRow->addWidget(m_metaUpBtn);
    metaEditRow->addWidget(m_metaDownBtn);
    metaEditRow->addStretch(1);
    m_metaTemplateBtn = new QPushButton(tr("Apply Template"), vcBox);
    m_metaTemplateBtn->setToolTip(tr("Fill in the standard RF tags (RF_TOTAL_SAMPLES, RF_SAMPLE_RATE, ...) derived from the probe context for RF captures, plus blank ingest rows (PROJECT, TAPE_ID, ...) for any FLAC. Only missing keys are added — review then Save."));
    m_metaReloadBtn = new QPushButton(tr("Reload"), vcBox);
    m_metaSaveBtn = new QPushButton(tr("Save to file"), vcBox);
    m_metaSaveBtn->setToolTip(tr("Write the comments back to the source FLAC in place."));
    metaEditRow->addWidget(m_metaTemplateBtn);
    metaEditRow->addWidget(m_metaReloadBtn);
    metaEditRow->addWidget(m_metaSaveBtn);
    vcLay->addLayout(metaEditRow);

    // Quick-add field row: an editable combo of common ingest presets (or
    // type any custom name) + "Add Field" inserts a blank row into the table.
    // Typed spaces become underscores; names are upper-cased and validated.
    auto* addFieldRow = new QHBoxLayout();
    auto* addFieldLbl = new QLabel(tr("Add field:"), vcBox);
    m_metaFieldCombo = new QComboBox(vcBox);
    m_metaFieldCombo->setEditable(true);
    m_metaFieldCombo->setInsertPolicy(QComboBox::NoInsert);
    for (const char* preset : { "TAPE_SPEED", "TAPE_FORMAT", "MACHINE",
                                "DATE_RECORDED", "CONDITION", "SOURCE" })
        m_metaFieldCombo->addItem(QString::fromLatin1(preset));
    m_metaFieldCombo->setToolTip(tr("Pick a common field (TAPE_SPEED, TAPE_FORMAT, MACHINE, DATE_RECORDED, CONDITION, SOURCE) or type any name — spaces become underscores. Click Add Field (or press Enter) to insert a blank row."));
    m_metaAddFieldBtn = new QPushButton(tr("Add Field"), vcBox);
    m_metaAddFieldBtn->setToolTip(tr("Insert a blank row with this field name."));
    addFieldRow->addWidget(addFieldLbl);
    addFieldRow->addWidget(m_metaFieldCombo, 1);
    addFieldRow->addWidget(m_metaAddFieldBtn);
    vcLay->addLayout(addFieldRow);

    m_metaStatusLabel = new QLabel(tr("Load a FLAC file to edit its metadata."), vcBox);
    m_metaStatusLabel->setWordWrap(true);
    vcLay->addWidget(m_metaStatusLabel);
    metaLay->addWidget(vcBox);
    metaLay->addStretch(1);

    m_tabs->addTab(metaPage, tr("Metadata Editor"));
    setMetaEnabled(false);

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
    // Browse: pick the output folder for the current name (the browsed path
    // becomes the user's custom choice via textChanged).
    connect(m_outPathBrowseBtn, &QPushButton::clicked, this, &MainWindow::browseOutputFile);
    // Any user edit of the Output file box marks the path as custom (kept
    // until the box is cleared — which restores the auto path — or a new
    // load/finished conversion resets it); the plan recomputes live.
    connect(m_outPathEdit, &QLineEdit::textChanged, this, [this]() {
        m_outPathCustom = m_outPathEdit && !m_outPathEdit->text().trimmed().isEmpty();
        applyCut();
    });
    // --- Metadata Editor tab ---
    connect(m_metaAddBtn, &QPushButton::clicked, this, &MainWindow::addMetaRow);
    connect(m_metaRemoveBtn, &QPushButton::clicked, this, &MainWindow::removeMetaRow);
    connect(m_metaUpBtn, &QPushButton::clicked, this, &MainWindow::moveMetaRowUp);
    connect(m_metaDownBtn, &QPushButton::clicked, this, &MainWindow::moveMetaRowDown);
    connect(m_metaSaveBtn, &QPushButton::clicked, this, &MainWindow::saveMetadata);
    connect(m_metaReloadBtn, &QPushButton::clicked, this, &MainWindow::reloadMetadata);
    connect(m_metaTemplateBtn, &QPushButton::clicked, this, &MainWindow::applyTemplate);
    connect(m_metaAddFieldBtn, &QPushButton::clicked, this, &MainWindow::addFieldFromBox);
    connect(m_metaFieldCombo->lineEdit(), &QLineEdit::returnPressed,
            this, &MainWindow::addFieldFromBox);

    // Drag & drop: the window accepts file drops, and NO child may swallow
    // them. QLineEdits accept file drops by default (they insert the path as
    // text) — the Output File box at the top is exactly where a dropped file
    // naturally lands, and it would eat the drop instead of loading the
    // file. Opt EVERY line edit in every tab out (the time box was the only
    // one before; this also covers the batch/sync out-dir boxes and the
    // metadata quick-add combo's internal edit).
    setAcceptDrops(true);
    for (QLineEdit* le : findChildren<QLineEdit*>())
        le->setAcceptDrops(false);

    m_watcher = new QFutureWatcher<FcChopResult>(this);
    connect(m_watcher, &QFutureWatcher<FcChopResult>::finished,
            this, &MainWindow::onChopFinished);

    m_probeWatcher = new QFutureWatcher<FcProbe>(this);
    connect(m_probeWatcher, &QFutureWatcher<FcProbe>::finished,
            this, &MainWindow::onProbeFinished);

    m_metaWatcher = new QFutureWatcher<FcMetaResult>(this);
    connect(m_metaWatcher, &QFutureWatcher<FcMetaResult>::finished,
            this, &MainWindow::onMetaSaveFinished);
    m_net = new QNetworkAccessManager(this);

    if (!fc_sox_available()) {
        m_statusLabel->setText(tr("WARNING: SoX not found (bundled or PATH) — cutting will fail."));
    }
    maybeCheckForUpdates();
}

// Theme menu: apply Dark/Light immediately + persist the explicit choice
// (QSettings "theme/mode"). The tbc-tools ThemedApplication does the full
// one-click switch: the isDarkTheme property FIRST, then the stock palette,
// the Qt>=6.8 color-scheme override, a force-repaint of every widget, and a
// deferred second pass — so a single menu click always switches fully.
void MainWindow::applyThemeChoice(bool dark)
{
    if (dark)
        ThemeUi::applyStockDarkThemeToApp();
    else
        ThemeUi::applyStockLightThemeToApp();
    QSettings s;
    s.beginGroup(QStringLiteral("theme"));
    s.setValue(QStringLiteral("mode"),
               dark ? QStringLiteral("dark") : QStringLiteral("light"));
    s.endGroup();
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
    // The output-path box stays editable even while a cut runs (an edit
    // applies to the NEXT cut — the running one already captured its path);
    // it is only disabled with no file loaded or while probing.
    m_outPathEdit->setEnabled(enabled || (m_probeOk && !m_probing));
    m_outPathBrowseBtn->setEnabled(enabled || (m_probeOk && !m_probing));
}

void MainWindow::browse()
{
    if (m_probing || (m_metaWatcher && m_metaWatcher->isRunning()))
        return;
    const QString startDir = m_inPath.isEmpty() ? QDir::homePath() : QFileInfo(m_inPath).absolutePath();
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("Select capture file"), startDir,
        tr("RF captures (*.flac *.ldf *.oga *.ogg *.wav *.u8 *.u16 *.s8 *.s16 *.r8 *.r16 *.8u *.8s *.16u *.16s *.raw *.bin *.pcm *.lds)"
           ";;FLAC / Ogg FLAC files (*.flac *.ldf *.oga *.ogg);;All files (*)"));
    if (fn.isEmpty())
        return;
    loadFile(fn);
}

QString MainWindow::effectiveOutDir() const
{
    // The Output File box (top row) replaced the old Output Directory
    // field: the default destination is the loaded input file's folder (the
    // original sibling -cut.flac behaviour); any other destination is a
    // full path typed into the Output File box itself. Returns "" when no
    // file is loaded.
    if (m_inPath.isEmpty())
        return QString();
    return QFileInfo(m_inPath).absolutePath();
}

void MainWindow::browseOutputFile()
{
    if (!m_probeOk || !m_outPathEdit)
        return;
    // Pick the output folder for the CURRENT name (auto or the user's) —
    // the full path in the box moves into the chosen folder.
    const QString current = m_outPathEdit->text().trimmed();
    const QString name = QFileInfo(current).fileName();
    const QString startDir = QFileInfo(current).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select output directory"),
        QDir().exists(startDir) ? startDir : QDir::homePath());
    if (dir.isEmpty())
        return;
    const QString newName = name.isEmpty()
        ? QStringLiteral("output.flac")
        : name;
    const QString newPath = QDir(dir).filePath(newName);
    // Programmatic setText goes through textChanged, so the browsed path
    // becomes the user's custom choice (kept until cleared).
    m_outPathEdit->setText(QDir::toNativeSeparators(newPath));
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
    m_outPathCustom = false;
    if (m_outPathEdit) {
        QSignalBlocker b(m_outPathEdit);
        m_outPathEdit->clear();
        m_outPathEdit->setPlaceholderText(tr("(no file selected)"));
        m_outPathEdit->setEnabled(false);
    }
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

    startProbe();
}

void MainWindow::loadFileAndMarkers(const QString& file, double inPos, double outPos,
                                    bool unitsSamples)
{
    // CLI --gui pre-load. Stash the requested IN/OUT positions; they are
    // applied when the fresh-load probe finishes (onProbeFinished), where the
    // real rate is known for the --units samples conversion. An empty file is
    // a no-op (the GUI opens as today); the stash is never touched by a
    // normal Browse/drop (loadFile), so a --gui with no file cannot leak
    // markers onto a later user load.
    if (file.isEmpty())
        return;
    m_pendingInPos = inPos;
    m_pendingOutPos = outPos;
    m_pendingUnitsSamples = unitsSamples;
    loadFile(file);
}

void MainWindow::startProbe()
{
    // Run the probe off the GUI thread. For files with an unknown STREAMINFO
    // total this scans every FLAC frame header (reading the whole file), which
    // can take minutes on large captures — doing it on the GUI thread would
    // freeze the window. Show a busy indicator + status while it runs.
    // Assumes m_inPath is already set; does NOT unload (loadFile did that, and
    // a metadata-save refresh wants to keep the current IN/OUT markers).
    m_probing = true;
    setControlsEnabled(false);
    setMetaEnabled(false);
    m_progress->setRange(0, 0); // busy indicator (no % text while busy)
    m_progress->setTextVisible(false);
    m_statusLabel->setText(tr("Probing… (scanning frame headers if the total is unknown)"));

    const QString path = m_inPath;
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
        m_probeIsRefresh = false;
        setMetaStreamInfo();
        loadMetadata();
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
                {10000, "10 MSPS (HiFi FM)"},
                {16000, "16 MSPS (VHS experimental)"},
                {20000, "20 MSPS"},
                {24000, "24 MSPS"},
                {28600, "28.6 MSPS (8fsc)"},
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

    // Set the IN/OUT markers. On a fresh load they sit at each end of the tape
    // (IN=00:00:00, OUT=full real duration). On a refresh (a re-probe triggered
    // by a metadata save) the existing markers are kept but clamped to the new
    // total, so a user's IN/OUT selection survives editing RF_TOTAL_SAMPLES.
    // m_inSec/m_outSec are the single source of truth; we set them here, then
    // push to the slider + time box with signals blocked so no recompute fires.
    m_syncing = true;
    const bool refresh = m_probeIsRefresh;
    m_probeIsRefresh = false;
    if (m_probe.total_samples_known && m_totalSec > 0.0) {
        if (refresh) {
            // Clamp the existing markers into the new tape range (keep ≥0.1 s).
            if (m_inSec < 0.0) m_inSec = 0.0;
            if (m_outSec > m_totalSec) m_outSec = m_totalSec;
            if (m_outSec - m_inSec < 0.1) { m_inSec = 0.0; m_outSec = m_totalSec; }
        } else {
            m_inSec = 0.0;
            m_outSec = m_totalSec;
        }
    } else {
        m_inSec = 0.0;
        m_outSec = 0.0;
    }
    // CLI --gui pre-load: override the default full-tape markers with the
    // requested --in/--out positions (converted from real RF samples via the
    // probed real rate when --units samples was used). Only on a fresh load —
    // never on a metadata-save refresh. Mirrors the Set IN/OUT clamping rules
    // (>= 0, within the tape, >= 0.1 s span).
    if (!refresh
        && (!std::isnan(m_pendingInPos) || !std::isnan(m_pendingOutPos))) {
        const double rate = m_probe.real_rate_hz;
        const bool unitsSamples = m_pendingUnitsSamples;
        auto toSec = [unitsSamples, rate](double pos) {
            return unitsSamples ? pos / rate : pos;
        };
        double inS = std::isnan(m_pendingInPos) ? 0.0 : qMax(0.0, toSec(m_pendingInPos));
        double outS = std::isnan(m_pendingOutPos) ? m_totalSec : qMax(0.0, toSec(m_pendingOutPos));
        if (m_totalSec > 0.0) {
            inS = qMin(inS, m_totalSec);
            outS = qMin(outS, m_totalSec);
        }
        if (outS - inS < 0.1) {
            outS = inS + 0.1; // keep the 0.1 s minimum span
            if (m_totalSec > 0.0 && outS > m_totalSec) {
                inS = qMax(0.0, m_totalSec - 0.1);
                outS = m_totalSec;
            }
        }
        m_inSec = inS;
        m_outSec = outS;
        m_pendingInPos = std::numeric_limits<double>::quiet_NaN();
        m_pendingOutPos = std::numeric_limits<double>::quiet_NaN();
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
    setMetaStreamInfo();
    loadMetadata();
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
    if (m_metaWatcher && m_metaWatcher->isRunning()) {
        m_statusLabel->setText(tr("A metadata save is in progress — wait for it to finish."));
        return;
    }
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    QStringList localFiles;
    for (const QUrl& u : urls) {
        if (u.isLocalFile())
            localFiles.append(u.toLocalFile());
    }
    if (localFiles.isEmpty())
        return;
    e->acceptProposedAction();
    if (localFiles.size() == 1) {
        // A single dropped file: load it into the Chop tab for marker cuts —
        // EXCEPT when a queue tab is the active tab: one-by-one drag-and-drop
        // onto the Batch Task / Sync Edit tab adds to that queue (the natural
        // way to build a queue file by file). Accept anything that looks
        // like a capture: the core probe sniffs FLAC / WAV by magic header
        // (so unknown extensions with the right magic work) and maps the raw
        // PCM extensions. Anything else fails the probe with a clear error.
        if (m_tabs->currentWidget() == m_syncEditTab && m_syncEditTab) {
            const int added = m_syncEditTab->addInputFiles(localFiles);
            m_statusLabel->setText(added > 0
                ? tr("Added 1 file to the Sync Edit set.")
                : tr("File already in the Sync Edit set."));
            return;
        }
        if (m_tabs->currentWidget() == m_batchTab && m_batchTab) {
            // A run in flight refuses adds — say so instead of "already queued".
            if (m_batchTab->isBusy()) {
                m_statusLabel->setText(tr("A batch run is in progress — add more files after it finishes."));
                return;
            }
            const int added = m_batchTab->addInputFiles(localFiles);
            m_statusLabel->setText(added > 0
                ? tr("Added 1 file to the Batch Task queue.")
                : tr("File already in the Batch Task queue."));
            return;
        }
        // Loading into the Chop tab mid-cut would reset the GUI state under
        // the running cut (unloadFile clears the plan the finish handler
        // reports against) — wait for it. (Adding to the batch/sync queues
        // above is still fine while a cut runs: those tabs run their own
        // jobs.)
        if (m_watcher && m_watcher->isRunning()) {
            m_statusLabel->setText(tr("A cut is in progress — wait for it to finish before loading another file."));
            return;
        }
        loadFile(localFiles.first());
        return;
    }
    // Several dropped files: if the Sync Edit tab is the active tab, add
    // to the Sync Edit set (a full file set is the normal drop); otherwise
    // the Batch Task queue (lds-converter model).
    if (m_tabs->currentWidget() == m_syncEditTab && m_syncEditTab) {
        const int added = m_syncEditTab->addInputFiles(localFiles);
        m_tabs->setCurrentWidget(m_syncEditTab);
        m_statusLabel->setText(added > 0
            ? tr("Added %1 file(s) to the Sync Edit set.").arg(added)
            : tr("No new files added to the Sync Edit set (already there)."));
        return;
    }
    const int added = m_batchTab ? m_batchTab->addInputFiles(localFiles) : 0;
    m_tabs->setCurrentWidget(m_batchTab);
    m_statusLabel->setText(added > 0
        ? tr("Added %1 file(s) to the Batch Task queue.").arg(added)
        : tr("No new files added to the Batch Task queue (already queued)."));
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
        m_processBtn->setText(tr("Process FLAC")); // no conversion pending
        return;
    }
    if (m_probe.format >= 2 && m_probe.format <= 5) {
        // Headerless raw PCM: there is no header to report; the rate label
        // points at the filename (the <n>msps hint is the only rate source).
        static const char* kRawNames[] = { "", "", "raw u8", "raw s8", "raw u16", "raw s16" };
        const QString fmtName = QString::fromLatin1(kRawNames[m_probe.format]);
        m_headerRateLabel->setText(tr("raw PCM (%1, rate from filename)").arg(fmtName));
    } else if (m_probe.format == 6) {
        // Ogg FLAC (.ldf): the FLAC header rate is the /1000 convention value.
        m_headerRateLabel->setText(tr("%1 Hz (header, Ogg FLAC)").arg(m_probe.header_sample_rate));
    } else {
        m_headerRateLabel->setText(tr("%1 Hz (header)").arg(m_probe.header_sample_rate));
    }
    // Process button label: FLAC (0) and Ogg FLAC (6) are cut FLAC→FLAC;
    // anything else (WAV / raw PCM / packed .lds) is CONVERTED to FLAC.
    m_processBtn->setText((m_probe.format == 0 || m_probe.format == 6)
        ? tr("Process FLAC") : tr("Process to FLAC"));
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
        // The Ogg stream length is the normal, exact source for an .ldf — shown
        // as information, not as an amber caution like the recovery paths below.
        const bool infoTag = (m_probe.total_samples_from_ogg != 0);
        if (m_probe.total_samples_from_ogg)
            tag = tr(" (exact, from the Ogg stream)");
        else if (m_probe.total_samples_from_vorbis)
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
            m_totalLabel->setStyleSheet(infoTag ? QString() : QStringLiteral("color:#e8a040;"));
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
        if (m_outPathEdit) m_outPathEdit->setPlaceholderText(tr("(no file selected)"));
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
    // MISRC capture naming convention (shared helper — see stemutil.h).
    char buf[4096];
    const QString outDir = effectiveOutDir();
    const QByteArray outDirB = outDir.toUtf8();
    const QString stem = renamedOutputStem(m_inPath, outHeaderRateHz, outBits);
    const QByteArray stemB = stem.toUtf8();
    QString autoPath;
    if (fc_generate_output_path(m_inPath.toUtf8().constData(),
                                 outDirB.constData(),
                                 stemB.constData(), buf, sizeof(buf))) {
        autoPath = QString::fromUtf8(buf);
    } else {
        autoPath = m_inPath + QStringLiteral("-cut.flac");
    }
    // The Output file box carries the REAL path as text (the "easy rename"
    // form): the auto-derived path while untouched, the user's edit once
    // they type (m_outPathCustom; clearing the box restores the auto path).
    // A bare name (no directory) resolves against the effective output
    // directory — never the process CWD, where a raw relative path would
    // silently land (the "typed name not applied" case) — and a name without
    // an extension gets .flac appended (the cut is ALWAYS written as FLAC,
    // and SoX picks its output handler from the extension).
    QString custom;
    if (m_outPathCustom && m_outPathEdit)
        custom = m_outPathEdit->text().trimmed();
    if (!custom.isEmpty()) {
        if (!QFileInfo(custom).isAbsolute())
            custom = QDir(effectiveOutDir()).absoluteFilePath(custom);
        if (QFileInfo(custom).completeSuffix().isEmpty())
            custom += QStringLiteral(".flac");
        m_outPath = custom;
    } else {
        m_outPath = autoPath;
        // The untouched box shows the live auto path as REAL (black,
        // directly editable) text — a gray placeholder reads as a disabled
        // field. Blocked so the update can't loop back through textChanged.
        if (m_outPathEdit) {
            QSignalBlocker b(m_outPathEdit);
            m_outPathEdit->setText(QDir::toNativeSeparators(autoPath));
        }
    }

    m_inLabel->setText(secsToHms(m_inSec));
    m_outLabel->setText(secsToHms(m_outSec));
    m_durLabel->setText(secsToHms(lenSec));
    m_startSampLabel->setText(tr("%1  (@ %2 Hz)")
        .arg(ulongStr(m_plan.start_samples))
        .arg(m_plan.real_sample_rate_hz, 0, 'f', 0));
    m_lenSampLabel->setText(ulongStr(m_plan.length_samples));
    m_processBtn->setEnabled(true);
    const QString bitsText =
        (outBits == 0) ? tr("source depth")
      : (outBits == 6) ? tr("6-bit crush in 8-bit FLAC")
      : tr("%1-bit").arg(outBits);
    const QString modeText = modeDisplay(outHeaderRateHz);
    // Show the RESOLVED output file in the plan status — a typed bare name
    // resolves against the output directory, which is otherwise invisible
    // (the placeholder only shows the auto-derived path).
    m_statusLabel->setText(tr("Plan ready: %1 + %2 samples | output %3 | %4.\nOutput file: %5")
        .arg(ulongStr(m_plan.start_samples),
             ulongStr(m_plan.length_samples),
             modeText,
             bitsText,
             QDir::toNativeSeparators(m_outPath)));
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
    // A custom output path (typed in the Output File box) may name a
    // directory that doesn't exist yet — make it too.
    const QString outParent = QFileInfo(m_outPath).absolutePath();
    if (!QDir().exists(outParent) && !QDir().mkpath(outParent)) {
        m_statusLabel->setText(tr("Cannot create output directory: %1").arg(outParent));
        return;
    }

    if (QFile::exists(m_outPath)) {
        auto r = QMessageBox::question(this, tr("Overwrite?"),
            tr("Output file exists:\n%1\nOverwrite?").arg(m_outPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes)
            return;
    }

    // Pre-conversion output metadata (the Metadata Editor tab rows authored
    // while a convertible non-FLAC source was loaded): the rows are embedded
    // into the output FLAC by the core's post-cut tag rewrite. For a FLAC
    // source the pending set is cleared (its editor saves in place instead).
    if (m_probe.format != 0) {
        QVector<QByteArray> rows;
        for (int i = 0; i < m_metaTable->rowCount(); ++i) {
            const QTableWidgetItem* kIt = m_metaTable->item(i, 0);
            const QTableWidgetItem* vIt = m_metaTable->item(i, 1);
            const QString key = kIt ? kIt->text().trimmed() : QString();
            const QString val = vIt ? vIt->text() : QString();
            if (key.isEmpty() && val.isEmpty())
                continue; // blank row — skip
            if (key.isEmpty()) {
                m_statusLabel->setText(tr("Metadata row %1: field name is empty — enter a name or clear the row (Metadata Editor tab).").arg(i + 1));
                return;
            }
            static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_]+$"));
            if (!valid.match(key).hasMatch()) {
                m_statusLabel->setText(tr("Metadata row %1: \"%2\" is not a valid field name (A-Z a-z 0-9 _).").arg(i + 1).arg(key));
                return;
            }
            rows.append((key.toUpper() + QLatin1Char('=') + val).toUtf8());
        }
        QVector<const char*> ptrs;
        ptrs.reserve(rows.size());
        for (const QByteArray& b : rows)
            ptrs.append(b.constData());
        fc_set_output_comments(ptrs.constData(), uint32_t(ptrs.size()));
    } else {
        fc_set_output_comments(nullptr, 0); // nothing pending for a FLAC cut
    }

    setControlsEnabled(false);
    // Wall-clock the run for the finished-time status.
    m_cutStartMSecs = QDateTime::currentMSecsSinceEpoch();
    // Start real progress polling: SoX -S "overall:X.XX%" → fc_chop_get_progress().
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFormat(tr("%p%"));
    m_progress->setTextVisible(true);
    m_progressTimer->start();
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
    m_progressTimer->stop(); // SoX -S polling ends with the cut
    FcChopResult r = m_watcher->result();
    m_cancelBtn->setEnabled(false);
    m_progress->setRange(0, 100);
    m_progress->setFormat(tr("%p%"));
    // Total processing time of the finished cut (all outcomes report it).
    const QString elapsed = formatElapsedMSecs(
        QDateTime::currentMSecsSinceEpoch() - m_cutStartMSecs);

    if (m_cancelRequested) {
        // The sox child was killed mid-cut: remove the partial output file so
        // the next run's clobber-avoidance doesn't see a corrupt -cut.flac.
        m_cancelRequested = false;
        if (!m_outPath.isEmpty() && QFile::exists(m_outPath))
            QFile::remove(m_outPath);
        m_progress->setValue(0);
        m_progress->setTextVisible(false);
        m_statusLabel->setText(tr("Cancelled (after %1).").arg(elapsed));
    } else if (r.ok) {
        m_progress->setValue(100); // complete — leave 100% showing
        // Surface the core's non-fatal notes/warnings on success (the .lds/Ogg
        // window provenance notes, a tag-rewrite warning) — the old UI dropped
        // r.stderr entirely on success, hiding real problems (a failed tag
        // embed used to be invisible next to a dead .tmp-tagsplice file).
        const QString notes = QString::fromUtf8(r.stderr_buf).trimmed();
        if (notes.contains(QLatin1String("warning:")))
            QMessageBox::warning(this, tr("Cut finished with a warning"),
                tr("The cut itself succeeded:\n%1\n\n%2").arg(m_outPath, notes));
        m_statusLabel->setText(notes.isEmpty()
            ? tr("Done in %1. Output: %2").arg(elapsed, m_outPath)
            : tr("Done in %1. Output: %2\n%3").arg(elapsed, m_outPath, notes));
        // Non-FLAC input (WAV / raw PCM / packed .lds / Ogg .ldf): the cut
        // output is a freshly converted native FLAC. Load it so the Metadata
        // Editor can tag the output right away (the raw/packed source's tags
        // are not editable) and the Chop tab reflects the produced file.
        if (m_probe.format != 0) {
            const QString out = m_outPath;
            // The custom name (if any) was consumed by this cut — don't let
            // it hijack (and overwrite) the next cut of the loaded output.
            m_outPathCustom = false;
            if (m_outPathEdit) {
                QSignalBlocker b(m_outPathEdit);
                m_outPathEdit->clear();
            }
            loadFile(out);
            return; // startProbe owns the controls until the probe finishes
        }
    } else {
        m_progress->setValue(0);
        m_progress->setTextVisible(false);
        QString err = QString::fromUtf8(r.stderr_buf).trimmed();
        if (err.isEmpty())
            err = tr("(no stderr) sox exit code %1").arg(r.exit_code);
        m_statusLabel->setText(tr("FAILED (exit %1, after %2): %3")
            .arg(r.exit_code).arg(elapsed).arg(err));
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

// --- Metadata Editor tab ----------------------------------------------------

static QString formatBytes(quint64 bytes)
{
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0, tb = gb * 1024.0;
    if (bytes >= tb) return QStringLiteral("%1 TB").arg(bytes / tb, 0, 'f', 2);
    if (bytes >= gb) return QStringLiteral("%1 GB").arg(bytes / gb, 0, 'f', 2);
    if (bytes >= mb) return QStringLiteral("%1 MB").arg(bytes / mb, 0, 'f', 2);
    if (bytes >= kb) return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

// Parse the little-endian comments blob (u32 LE count + per-comment u32 LE
// len + "KEY=value" bytes) into (key, value) pairs, splitting on the first '='.
// Returns an empty vector on a malformed blob (the caller reports an error).
static QVector<QPair<QString, QString>> parseCommentsBlob(const QByteArray& blob)
{
    const uchar* p = reinterpret_cast<const uchar*>(blob.constData());
    const uchar* end = p + blob.size();
    auto rdU32 = [&p, end](bool& okb) -> quint32 {
        if (p + 4 > end) { okb = false; return 0; }
        const quint32 v = quint32(p[0]) | (quint32(p[1]) << 8)
                         | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
        p += 4;
        return v;
    };
    bool okb = true;
    const quint32 count = rdU32(okb);
    QVector<QPair<QString, QString>> out;
    out.reserve(int(count));
    for (quint32 i = 0; i < count && okb; ++i) {
        const quint32 len = rdU32(okb);
        if (!okb || p + len > end) { okb = false; break; }
        const QString kv = QString::fromUtf8(reinterpret_cast<const char*>(p), int(len));
        p += len;
        const int eq = kv.indexOf(QLatin1Char('='));
        out.append({eq >= 0 ? kv.left(eq) : kv, eq >= 0 ? kv.mid(eq + 1) : QString()});
    }
    if (!okb)
        return {};
    return out;
}

// Derive the capture date/time from the filename when it follows the
// DdD/ld-decode naming convention (e.g. RF-Sample_2022-12-11_00-00-34.lds):
// a YYYY-MM-DD token, optionally followed by _HH-MM-SS. Returns
// "YYYY-MM-DD" / "YYYY-MM-DD HH:MM:SS", or an empty string when the name
// carries no such context (or the values are not a real date/time).
static QString dateRecordedFromFilename(const QString& path)
{
    static const QRegularExpression re(QStringLiteral(
        "(\\d{4})-(\\d{2})-(\\d{2})(?:[_-](\\d{2})-(\\d{2})-(\\d{2}))?"));
    const auto m = re.match(QFileInfo(path).fileName());
    if (!m.hasMatch())
        return QString();
    const int y = m.captured(1).toInt();
    const int mo = m.captured(2).toInt();
    const int d = m.captured(3).toInt();
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return QString();
    if (!m.captured(4).isNull()) {
        const int h = m.captured(4).toInt();
        const int mi = m.captured(5).toInt();
        const int s = m.captured(6).toInt();
        if (h > 23 || mi > 59 || s > 59)
            return QString();
        return QStringLiteral("%1-%2-%3 %4:%5:%6")
            .arg(y, 4, 10, QLatin1Char('0'))
            .arg(mo, 2, 10, QLatin1Char('0'))
            .arg(d, 2, 10, QLatin1Char('0'))
            .arg(h, 2, 10, QLatin1Char('0'))
            .arg(mi, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1-%2-%3")
        .arg(y, 4, 10, QLatin1Char('0'))
        .arg(mo, 2, 10, QLatin1Char('0'))
        .arg(d, 2, 10, QLatin1Char('0'));
}

void MainWindow::setMetaStreamInfo()
{
    // Read-only STREAMINFO summary at the top of the editor page.
    static const char* kFmtNames[] = { "FLAC", "PCM WAV", "raw u8", "raw s8", "raw u16", "raw s16", "Ogg FLAC", "Packed .lds (10-bit)" };
    if (!m_probeOk) {
        for (auto* lbl : {m_metaFormatLabel, m_metaHeaderRateLabel, m_metaBitsChLabel,
                          m_metaRealRateLabel, m_metaTotalSamplesLabel, m_metaFileSizeLabel})
            lbl->setText(QStringLiteral("—"));
        return;
    }
    m_metaFormatLabel->setText((m_probe.format <= 7)
        ? QString::fromLatin1(kFmtNames[m_probe.format])
        : tr("unknown"));
    if (m_probe.format >= 2 && m_probe.format <= 5)
        m_metaHeaderRateLabel->setText(tr("raw PCM (rate from filename)"));
    else if (m_probe.format == 7)
        m_metaHeaderRateLabel->setText(tr("packed 10-bit (rate from filename)"));
    else
        m_metaHeaderRateLabel->setText(tr("%1 Hz").arg(m_probe.header_sample_rate));
    m_metaBitsChLabel->setText(tr("%1-bit / %2 ch")
        .arg(m_probe.bits_per_sample).arg(m_probe.channels));
    if (m_probe.is_rf)
        m_metaRealRateLabel->setText(tr("%1 Hz (RF, %2 MSPS)")
            .arg(m_probe.real_rate_hz, 0, 'f', 0).arg(m_probe.msps, 0, 'f', 0));
    else
        m_metaRealRateLabel->setText(tr("%1 Hz").arg(m_probe.real_rate_hz, 0, 'f', 0));
    if (m_probe.total_samples_known)
        m_metaTotalSamplesLabel->setText(tr("%1 (declared %2)")
            .arg(ulongStr(m_probe.total_samples)).arg(ulongStr(m_probe.declared_total_samples)));
    else
        m_metaTotalSamplesLabel->setText(tr("unknown (no STREAMINFO total)"));
    m_metaFileSizeLabel->setText(tr("%1 (%2 bytes)")
        .arg(formatBytes(m_probe.file_size)).arg(ulongStr(m_probe.file_size)));
}

void MainWindow::setMetaEnabled(bool on)
{
    // Only FLAC files carry an editable Vorbis comment block.
    const bool editable = on && m_probeOk && m_probe.format == 0;
    m_metaTable->setEnabled(editable);
    m_metaAddBtn->setEnabled(editable);
    m_metaRemoveBtn->setEnabled(editable);
    m_metaUpBtn->setEnabled(editable);
    m_metaDownBtn->setEnabled(editable);
    m_metaReloadBtn->setEnabled(editable);
    m_metaSaveBtn->setEnabled(editable);
    // The blank ingest rows are useful for any FLAC; the RF rate/total tags
    // only apply to RF captures (the template core yields none for non-RF),
    // so the button stays enabled for every loaded FLAC.
    m_metaTemplateBtn->setEnabled(editable);
    m_metaFieldCombo->setEnabled(editable);
    m_metaAddFieldBtn->setEnabled(editable);
}

void MainWindow::loadMetadata()
{
    // Read every Vorbis comment from the source FLAC via the packed-blob FFI
    // and fill the editor table. A convertible non-FLAC source (WAV / raw /
    // packed .lds / Ogg .ldf) cannot carry comments, so the editor instead
    // becomes the authoring surface for the OUTPUT's metadata. No file →
    // cleared + disabled.
    if (!m_probeOk || m_inPath.isEmpty() || m_probe.format != 0) {
        m_metaTable->setRowCount(0);
        setMetaEnabled(false);
        if (m_probeOk && !m_inPath.isEmpty()) {
            populatePreConversionMetadata();
            return;
        }
        m_metaStatusLabel->setText(tr("Load a FLAC file to edit its metadata."));
        return;
    }

    const QByteArray pathB = m_inPath.toUtf8();
    QByteArray err(256, '\0');
    const uintptr_t size = fc_comments_blob_size(pathB.constData(), err.data(), err.size());
    if (size == 0) {
        m_metaTable->setRowCount(0);
        setMetaEnabled(false);
        m_metaStatusLabel->setText(tr("Could not read metadata: %1").arg(QString::fromUtf8(err)));
        return;
    }
    QByteArray blob(int(size), '\0');
    QByteArray err2(256, '\0');
    const int ok = fc_read_comments_blob(pathB.constData(), blob.data(), blob.size(),
                                         err2.data(), err2.size());
    if (!ok) {
        m_metaTable->setRowCount(0);
        setMetaEnabled(false);
        m_metaStatusLabel->setText(tr("Could not read metadata: %1").arg(QString::fromUtf8(err2)));
        return;
    }

    // Parse the little-endian blob via the shared helper.
    const auto pairs = parseCommentsBlob(blob);
    if (pairs.isEmpty() && blob.size() >= 4) {
        // A non-empty blob that parsed to zero pairs is malformed (count said
        // there were entries but the body was truncated). A 4-byte count=0
        // blob is legit (a tag-less file) and yields zero pairs.
        const uchar* pp = reinterpret_cast<const uchar*>(blob.constData());
        const quint32 declared = quint32(pp[0]) | (quint32(pp[1]) << 8)
                                | (quint32(pp[2]) << 16) | (quint32(pp[3]) << 24);
        if (declared != 0) {
            m_metaTable->setRowCount(0);
            setMetaEnabled(false);
            m_metaStatusLabel->setText(tr("Could not read metadata: malformed comment blob."));
            return;
        }
    }
    m_metaTable->setRowCount(int(pairs.size()));
    for (int i = 0; i < pairs.size(); ++i) {
        m_metaTable->setItem(i, 0, new QTableWidgetItem(pairs[i].first));
        m_metaTable->setItem(i, 1, new QTableWidgetItem(pairs[i].second));
    }
    // Auto-populate the capture date/time from the filename context (the
    // DdD/ld-decode naming convention embeds it: e.g.
    // RF-Sample_2022-12-11_00-00-34.lds). Only when the tag is not already
    // present (a re-load after Save must not duplicate it); the row lands
    // in the table for review — Save writes it to the file.
    QString autoNote;
    const QString recorded = dateRecordedFromFilename(m_inPath);
    if (!recorded.isEmpty() && !metaHasKey(QStringLiteral("DATE_RECORDED"))) {
        const int row = m_metaTable->rowCount();
        m_metaTable->insertRow(row);
        m_metaTable->setItem(row, 0, new QTableWidgetItem(QStringLiteral("DATE_RECORDED")));
        m_metaTable->setItem(row, 1, new QTableWidgetItem(recorded));
        autoNote = tr(" Auto-populated DATE_RECORDED from the filename.");
    }
    m_metaStatusLabel->setText(tr("%1 comment(s) loaded. Edit fields, then Save to write in place.%2")
        .arg(pairs.size())
        .arg(autoNote));
    setMetaEnabled(true);
}

// A convertible non-FLAC source (WAV / raw PCM / packed .lds / Ogg .ldf)
// cannot carry Vorbis comments, but its conversion output can: the editor
// becomes the authoring surface for the OUTPUT's metadata — the RF template
// tags from the probe, the capture date/time from the SOURCE's filename (so
// the output carries the source's context even when it is renamed), and the
// standard ingest fields to fill. Process embeds the rows into the output
// (fc_set_output_comments); Save/Reload stay off (the tag-less source has
// nothing to write or re-read).
void MainWindow::populatePreConversionMetadata()
{
    // The rows are editable; Save/Reload target a real file, so they stay
    // disabled for a tag-less source.
    m_metaTable->setEnabled(true);
    m_metaAddBtn->setEnabled(true);
    m_metaRemoveBtn->setEnabled(true);
    m_metaUpBtn->setEnabled(true);
    m_metaDownBtn->setEnabled(true);
    m_metaTemplateBtn->setEnabled(true);
    m_metaFieldCombo->setEnabled(true);
    m_metaAddFieldBtn->setEnabled(true);
    m_metaReloadBtn->setEnabled(false);
    m_metaSaveBtn->setEnabled(false);

    int added = 0;
    // 1. RF template tags from the probe (RF_TOTAL_SAMPLES, RF_SAMPLE_RATE,
    //    ... for RF captures; none for plain audio) — same merge rules as
    //    Apply Template (only missing keys; the core's post-cut rewrite
    //    writes the authoritative numbers into the output anyway).
    QByteArray blob(4096, '\0');
    QByteArray err(256, '\0');
    const uintptr_t n = fc_rf_template_from_probe(&m_probe, blob.data(), blob.size(),
                                                  err.data(), err.size());
    if (n == 0) {
        m_metaStatusLabel->setText(tr("Preparing output metadata failed: %1")
            .arg(QString::fromUtf8(err)));
        return;
    }
    blob.resize(int(n));
    for (const auto& kv : parseCommentsBlob(blob)) {
        if (!metaHasKey(kv.first)) {
            const int row = m_metaTable->rowCount();
            m_metaTable->insertRow(row);
            m_metaTable->setItem(row, 0, new QTableWidgetItem(kv.first));
            m_metaTable->setItem(row, 1, new QTableWidgetItem(kv.second));
            ++added;
        }
    }
    // 2. The capture date/time from the SOURCE's filename (the DdD/ld-decode
    //    naming convention, e.g. RF-Sample_2022-12-11_00-00-34.lds).
    const QString recorded = dateRecordedFromFilename(m_inPath);
    if (!recorded.isEmpty() && !metaHasKey(QStringLiteral("DATE_RECORDED"))) {
        const int row = m_metaTable->rowCount();
        m_metaTable->insertRow(row);
        m_metaTable->setItem(row, 0, new QTableWidgetItem(QStringLiteral("DATE_RECORDED")));
        m_metaTable->setItem(row, 1, new QTableWidgetItem(recorded));
        ++added;
    }
    // 3. Blank ingest fields to fill (the same set as Apply Template).
    static const char* kIngestFields[] = { "PROJECT", "TAPE_ID", "OPERATOR", "LOCATION", "NOTES" };
    for (const char* k : kIngestFields) {
        const QString key = QString::fromLatin1(k);
        if (!metaHasKey(key)) {
            const int row = m_metaTable->rowCount();
            m_metaTable->insertRow(row);
            m_metaTable->setItem(row, 0, new QTableWidgetItem(key));
            m_metaTable->setItem(row, 1, new QTableWidgetItem(QString()));
            ++added;
        }
    }
    m_metaStatusLabel->setText(tr(
        "Output metadata prepared: %1 row(s) — RF tags from the probe, DATE_RECORDED from the "
        "source file name, plus ingest fields to fill. Edit them, then Process: they are embedded "
        "into the output FLAC (this %2 source cannot carry tags, so Save is off).")
        .arg(added)
        .arg(m_metaFormatLabel->text()));
}

void MainWindow::reloadMetadata()
{
    if (!m_probeOk || m_inPath.isEmpty() || m_probe.format != 0)
        return;
    loadMetadata();
}

void MainWindow::saveMetadata()
{
    if (!m_probeOk || m_inPath.isEmpty() || m_probe.format != 0)
        return;
    if (m_probing || (m_watcher && m_watcher->isRunning()) || (m_metaWatcher && m_metaWatcher->isRunning()))
        return;

    // Gather non-empty rows into "KEY=value" C strings. A fully-blank row is
    // skipped (a no-op); a row with an empty field name but a value is an error.
    QVector<QByteArray> commentBavs;
    for (int i = 0; i < m_metaTable->rowCount(); ++i) {
        const QTableWidgetItem* kIt = m_metaTable->item(i, 0);
        const QTableWidgetItem* vIt = m_metaTable->item(i, 1);
        const QString key = kIt ? kIt->text().trimmed() : QString();
        const QString val = vIt ? vIt->text() : QString();
        if (key.isEmpty() && val.isEmpty())
            continue; // blank row — skip
        if (key.isEmpty()) {
            m_metaStatusLabel->setText(tr("Row %1: field name is empty — enter a name or clear the row.").arg(i + 1));
            return;
        }
        commentBavs.append((key + QLatin1Char('=') + val).toUtf8());
    }

    setMetaEnabled(false);
    m_progress->setRange(0, 0); // busy indicator (shared with the Chop page)
    m_progress->setTextVisible(false);
    m_metaStatusLabel->setText(tr("Saving metadata…"));
    m_statusLabel->setText(tr("Saving metadata to %1…").arg(m_inPath));

    const QString path = m_inPath;
    auto fut = QtConcurrent::run([path, commentBavs]() -> FcMetaResult {
        FcMetaResult r;
        QByteArray pathB = path.toUtf8();
        QByteArray err(256, '\0');
        // Build the C string pointer array inside the lambda so the pointers
        // reference the lambda's own QByteArray copy (captured by value).
        QVector<const char*> ptrs;
        ptrs.reserve(commentBavs.size());
        for (const QByteArray& b : commentBavs)
            ptrs.append(b.constData());
        const int ok = fc_replace_comments(pathB.constData(), ptrs.constData(),
                                           uint32_t(ptrs.size()),
                                           err.data(), err.size());
        r.ok = (ok != 0);
        r.error = QString::fromUtf8(err);
        return r;
    });
    m_metaWatcher->setFuture(fut);
}

void MainWindow::onMetaSaveFinished()
{
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    const FcMetaResult r = m_metaWatcher->result();
    if (r.ok) {
        m_metaStatusLabel->setText(tr("Metadata saved. Re-probing to refresh both tabs…"));
        m_statusLabel->setText(tr("Metadata saved to %1.").arg(m_inPath));
        // Re-probe so the Chop page reflects any changed RF_TOTAL_SAMPLES /
        // RF_SAMPLE_RATE, and reload the editor from the freshly written file.
        // The refresh keeps the current IN/OUT markers (clamped to the new total).
        m_probeIsRefresh = true;
        startProbe();
    } else {
        const QString err = r.error.trimmed().isEmpty() ? tr("unknown error") : r.error.trimmed();
        m_metaStatusLabel->setText(tr("Save failed: %1").arg(err));
        m_statusLabel->setText(tr("Metadata save failed: %1").arg(err));
        setMetaEnabled(true);
    }
}

void MainWindow::addMetaRow()
{
    const int row = m_metaTable->rowCount();
    m_metaTable->insertRow(row);
    m_metaTable->setItem(row, 0, new QTableWidgetItem(QString()));
    m_metaTable->setItem(row, 1, new QTableWidgetItem(QString()));
    m_metaTable->setCurrentCell(row, 0);
    m_metaTable->editItem(m_metaTable->item(row, 0));
}

void MainWindow::removeMetaRow()
{
    const int row = m_metaTable->currentRow();
    if (row < 0)
        return;
    m_metaTable->removeRow(row);
}

void MainWindow::moveMetaRowUp()
{
    const int row = m_metaTable->currentRow();
    if (row <= 0)
        return;
    for (int col = 0; col < 2; ++col) {
        QTableWidgetItem* a = m_metaTable->takeItem(row, col);
        QTableWidgetItem* b = m_metaTable->takeItem(row - 1, col);
        m_metaTable->setItem(row, col, b);
        m_metaTable->setItem(row - 1, col, a);
    }
    m_metaTable->setCurrentCell(row - 1, 0);
}

void MainWindow::moveMetaRowDown()
{
    const int row = m_metaTable->currentRow();
    if (row < 0 || row >= m_metaTable->rowCount() - 1)
        return;
    for (int col = 0; col < 2; ++col) {
        QTableWidgetItem* a = m_metaTable->takeItem(row, col);
        QTableWidgetItem* b = m_metaTable->takeItem(row + 1, col);
        m_metaTable->setItem(row, col, b);
        m_metaTable->setItem(row + 1, col, a);
    }
    m_metaTable->setCurrentCell(row + 1, 0);
}

bool MainWindow::metaHasKey(const QString& key) const
{
    // Vorbis field names are case-insensitive; the editor upper-cases on save.
    const QString up = key.trimmed().toUpper();
    if (up.isEmpty())
        return true; // treat blank as "present" so we never add an empty-key row
    for (int i = 0; i < m_metaTable->rowCount(); ++i) {
        const QTableWidgetItem* it = m_metaTable->item(i, 0);
        if (it && it->text().trimmed().toUpper() == up)
            return true;
    }
    return false;
}

void MainWindow::applyTemplate()
{
    // "Apply Template" — fill the editor with the standard RF tags derived
    // from the probe context (RF_TOTAL_SAMPLES, RF_SAMPLE_RATE,
    // RF_SAMPLE_RATE_KHZ, DURATION_SECONDS, LENGTH) plus blank ingest rows
    // (PROJECT, TAPE_ID, OPERATOR, LOCATION, NOTES) for the user to fill in.
    // Any loaded FLAC is eligible; RF rate/total tags are only produced for
    // RF captures (the template core yields none for non-RF files). Merge
    // semantics: only keys that are NOT already present are added; existing
    // values are left untouched. The rows land in the table for review — the
    // user hits Save to write them.
    if (!m_probeOk || m_inPath.isEmpty()) {
        m_metaStatusLabel->setText(tr("Apply Template needs a loaded file."));
        return;
    }
    if (m_metaWatcher && m_metaWatcher->isRunning())
        return;

    QByteArray blob(4096, '\0');
    QByteArray err(256, '\0');
    const uintptr_t n = fc_rf_template_from_probe(&m_probe, blob.data(), blob.size(),
                                                   err.data(), err.size());
    if (n == 0) {
        m_metaStatusLabel->setText(tr("Apply Template failed: %1").arg(QString::fromUtf8(err)));
        return;
    }
    blob.resize(int(n));
    const auto tmpl = parseCommentsBlob(blob);
    // parseCommentsBlob returns empty on a malformed body OR a legit count=0
    // blob; distinguish by re-reading the declared count.
    if (tmpl.isEmpty() && n >= 4) {
        const uchar* pp = reinterpret_cast<const uchar*>(blob.constData());
        const quint32 declared = quint32(pp[0]) | (quint32(pp[1]) << 8)
                                | (quint32(pp[2]) << 16) | (quint32(pp[3]) << 24);
        if (declared != 0) {
            m_metaStatusLabel->setText(tr("Apply Template failed: malformed template blob."));
            return;
        }
    }

    int added = 0;
    // 1. Computed RF tags (merge: only missing keys).
    for (const auto& kv : tmpl) {
        if (!metaHasKey(kv.first)) {
            const int row = m_metaTable->rowCount();
            m_metaTable->insertRow(row);
            m_metaTable->setItem(row, 0, new QTableWidgetItem(kv.first));
            m_metaTable->setItem(row, 1, new QTableWidgetItem(kv.second));
            ++added;
        }
    }
    // 2. Blank ingest-metadata rows for the user to fill (merge: only missing).
    static const char* kIngestFields[] = { "PROJECT", "TAPE_ID", "OPERATOR", "LOCATION", "NOTES" };
    for (const char* k : kIngestFields) {
        const QString key = QString::fromLatin1(k);
        if (!metaHasKey(key)) {
            const int row = m_metaTable->rowCount();
            m_metaTable->insertRow(row);
            m_metaTable->setItem(row, 0, new QTableWidgetItem(key));
            m_metaTable->setItem(row, 1, new QTableWidgetItem(QString()));
            ++added;
        }
    }
    if (added == 0)
        m_metaStatusLabel->setText(tr("Template: all standard tags already present — nothing added."));
    else
        m_metaStatusLabel->setText(tr("Template applied: %1 row(s) added. Review the values, then Save to write.").arg(added));
}

void MainWindow::addFieldFromBox()
{
    // "Add Field" — insert a blank row with the name picked/typed in the
    // quick-add box. Whitespace becomes underscores, the name is upper-cased
    // and validated (editor convention: letters, digits, _). A name already
    // in the table is reported instead of duplicated; the row is only saved
    // to the file when the user hits Save.
    if (!m_metaFieldCombo->isEnabled())
        return;
    QString key = m_metaFieldCombo->currentText().trimmed();
    key.replace(QLatin1Char(' '), QLatin1Char('_'));
    key = key.toUpper();
    if (key.isEmpty()) {
        m_metaStatusLabel->setText(tr("Add Field: type a field name first."));
        return;
    }
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_]+$"));
    if (!valid.match(key).hasMatch()) {
        m_metaStatusLabel->setText(tr("Add Field: \"%1\" is not a valid field name (use letters, digits, _).").arg(key));
        return;
    }
    if (metaHasKey(key)) {
        m_metaStatusLabel->setText(tr("Add Field: %1 already present — nothing added.").arg(key));
        return;
    }
    const int row = m_metaTable->rowCount();
    m_metaTable->insertRow(row);
    m_metaTable->setItem(row, 0, new QTableWidgetItem(key));
    m_metaTable->setItem(row, 1, new QTableWidgetItem(QString()));
    m_metaTable->setCurrentCell(row, 0);
    m_metaTable->editItem(m_metaTable->item(row, 1));
    m_metaStatusLabel->setText(tr("Add Field: %1 row added — fill the value, then Save to write.").arg(key));
    // Consume the picked/typed name so the box is ready for the next field.
    m_metaFieldCombo->setCurrentIndex(-1);
    m_metaFieldCombo->lineEdit()->clear();
}
