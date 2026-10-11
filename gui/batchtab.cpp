#include "batchtab.h"

#include "stemutil.h"
#include "flacchop.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <future>
#include <utility>
#include <vector>

// Per-job outcome, collected from the futures in queue order.
namespace {
struct BatchResult {
    bool ok = false;
    bool cancelled = false;
    QString message; // Done: output path / Failed: reason
    qint64 elapsedMSecs = 0; // wall-clock processing time of the job
    QString verify; // the "verify: …" verdict line from the notes, if any
    bool deletedSource = false; // opt-in delete-after-PASS removed the input
};

// Stamps the job's wall-clock processing time into `out` at EVERY exit
// (RAII — the job lambda has many return paths).
struct ElapsedStamp {
    qint64 t0;
    qint64* out;
    ~ElapsedStamp() { *out = QDateTime::currentMSecsSinceEpoch() - t0; }
};

QString normalizeInputPath(const QString& raw)
{
    QString t = raw.trimmed();
    if (t.isEmpty())
        return QString();
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(t));
    // Absolute, so every later path comparison (skip-existing, the
    // never-write-onto-an-input protection, output naming) is consistent no
    // matter how the file was added (dialog, drag&drop, CLI-relative).
    return QFileInfo(clean).absoluteFilePath();
}

// The canonical output path a fresh whole-file run would produce for `input`:
// <outdir-or-input-dir>/<renamed-stem | input-stem>-cut.flac. Mirrors the
// Rust generate_output_path default naming (minus the on-disk clobber
// avoidance, which batch handles itself so skip/overwrite checks are
// deterministic and race-free between parallel jobs).
QString canonicalOutputPath(const QString& input, const QString& outDir,
                             quint64 modeHz, uint bits)
{
    const QFileInfo info(input);
    const QString stem = renamedOutputStem(input, modeHz, bits);
    const QString name = stem.isEmpty()
        ? info.completeBaseName() + QStringLiteral("-cut.flac")
        : stem + QStringLiteral(".flac");
    const QString dir = outDir.isEmpty() ? info.absolutePath() : outDir;
    return QDir(dir).filePath(name);
}

// Reserve `path` for this batch (lds-converter model): a path already claimed
// by an earlier job in the SAME run gets `_2`, `_3`, … appended to the stem
// so two inputs that map to the same name never overwrite each other.
QString reserveUnique(const QString& path, QSet<QString>& reserved)
{
    if (!reserved.contains(path.toLower())) {
        reserved.insert(path.toLower());
        return path;
    }
    const QFileInfo info(path);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix = info.completeSuffix();
    for (int n = 2;; ++n) {
        const QString cand = QDir(dir).filePath(
            suffix.isEmpty()
                ? QStringLiteral("%1_%2").arg(stem).arg(n)
                : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix));
        if (!reserved.contains(cand.toLower())) {
            reserved.insert(cand.toLower());
            return cand;
        }
    }
}

QString modeDisplay(quint64 headerRateHz)
{
    switch (headerRateHz) {
    case 10000: return QStringLiteral("10 MSPS (HiFi FM)");
    case 16000: return QStringLiteral("16 MSPS (VHS experimental)");
    case 20000: return QStringLiteral("20 MSPS");
    case 24000: return QStringLiteral("24 MSPS");
    case 28600: return QStringLiteral("28.6 MSPS (8fsc)");
    default: return QStringLiteral("source rate");
    }
}
} // namespace

BatchTab::BatchTab(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
    refreshOutDirPlaceholder();
    // Restore the persisted verify preference (default on — data-handling
    // safety net for the whole-file batch workflow).
    {
        QSettings s;
        m_verifyCheck->setChecked(s.value(QStringLiteral("batch/verify"), true).toBool());
        m_deleteAfterVerifyCheck->setChecked(
            s.value(QStringLiteral("batch/deleteAfterVerify"), false).toBool());
    }
    if (!fc_sox_available())
        m_statusLabel->setText(tr("WARNING: SoX not found (bundled or PATH) — processing will fail."));
}

void BatchTab::buildUi()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    // --- Input queue ---
    auto* queueBox = new QGroupBox(tr("Input Queue"), this);
    auto* queueLay = new QVBoxLayout(queueBox);
    m_queue = new QTableWidget(0, 2, queueBox);
    m_queue->setHorizontalHeaderLabels({tr("Input file"), tr("Status")});
    m_queue->verticalHeader()->setVisible(false);
    m_queue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queue->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queue->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_queue->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_queue->setToolTip(tr("Files queued for batch processing. Each file is processed whole with the settings below. Drop several files anywhere on the window to add them here."));
    queueLay->addWidget(m_queue);

    auto* queueBtnRow = new QHBoxLayout();
    m_addBtn = new QPushButton(tr("Add…"), queueBox);
    m_removeBtn = new QPushButton(tr("Remove selected"), queueBox);
    m_clearBtn = new QPushButton(tr("Clear"), queueBox);
    m_addBtn->setToolTip(tr("Add capture files to the queue (multi-select)."));
    m_removeBtn->setToolTip(tr("Remove the selected rows from the queue."));
    m_clearBtn->setToolTip(tr("Clear the whole queue."));
    m_queueSummaryLabel = new QLabel(tr("Queued files: 0"), queueBox);
    queueBtnRow->addWidget(m_addBtn);
    queueBtnRow->addWidget(m_removeBtn);
    queueBtnRow->addWidget(m_clearBtn);
    queueBtnRow->addStretch(1);
    queueBtnRow->addWidget(m_queueSummaryLabel);
    queueLay->addLayout(queueBtnRow);
    lay->addWidget(queueBox);

    // --- Output directory ---
    auto* outDirBox = new QGroupBox(tr("Output Directory"), this);
    auto* outDirLay = new QHBoxLayout(outDirBox);
    m_outDirEdit = new QLineEdit(outDirBox);
    m_outDirEdit->setToolTip(tr("Outputs are written here. Empty = each input file's own folder."));
    m_outDirBrowseBtn = new QPushButton(tr("Browse..."), outDirBox);
    outDirLay->addWidget(m_outDirEdit, 1);
    outDirLay->addWidget(m_outDirBrowseBtn, 0);
    lay->addWidget(outDirBox);

    // --- Output processing settings (whole-file re-compression) ---
    auto* setBox = new QGroupBox(tr("Output Processing (applied to every queued file)"), this);
    auto* setLay = new QFormLayout(setBox);
    m_modeCombo = new QComboBox(setBox);
    m_modeCombo->addItem(tr("Keep source rate"), quint64(0));
    m_modeCombo->addItem(tr("10 MSPS (HiFi FM)"), quint64(10000));
    m_modeCombo->addItem(tr("16 MSPS (VHS experimental)"), quint64(16000));
    m_modeCombo->addItem(tr("20 MSPS"), quint64(20000));
    m_modeCombo->addItem(tr("24 MSPS"), quint64(24000));
    m_modeCombo->addItem(tr("28.6 MSPS (8fsc)"), quint64(28600));
    m_bitsCombo = new QComboBox(setBox);
    m_bitsCombo->addItem(tr("Keep source bit-depth"), uint(0));
    m_bitsCombo->addItem(tr("8-bit"), uint(8));
    m_bitsCombo->addItem(tr("12-bit (MISRC true 12-bit FLAC)"), uint(12));
    m_bitsCombo->addItem(tr("6-bit crush (stored as 8-bit FLAC)"), uint(6));
    m_filterCheck = new QCheckBox(tr("Apply basic RF filter profile"), setBox);
    m_filterCheck->setChecked(true);
    m_filterCheck->setToolTip(tr("The sinc low-pass profile for the selected output rate. Only used when an output rate mode is selected."));
    setLay->addRow(tr("Output mode:"), m_modeCombo);
    setLay->addRow(tr("Bit-depth:"), m_bitsCombo);
    setLay->addRow(QString(), m_filterCheck);
    lay->addWidget(setBox);

    // --- Run options ---
    auto* optBox = new QGroupBox(tr("Batch Options"), this);
    auto* optLay = new QFormLayout(optBox);
    m_parallelCheck = new QCheckBox(tr("Parallel processing"), optBox);
    m_parallelCheck->setChecked(true);
    m_parallelCheck->setToolTip(tr("Run queued files concurrently (batch of more than one file), up to %1 jobs at once. Unchecked = one file at a time. Every job runs on a worker thread either way, so Stop always works.")
        .arg(std::max(1, QThread::idealThreadCount())));
    m_overwriteCheck = new QCheckBox(tr("Overwrite existing outputs"), optBox);
    m_overwriteCheck->setToolTip(tr("Unchecked: a file whose output already exists is skipped. Checked: the existing output is replaced."));
    m_verifyCheck = new QCheckBox(tr("Verify output (bit-perfect, before deleting sources)"), optBox);
    m_verifyCheck->setChecked(true);
    m_verifyCheck->setToolTip(tr("After each PLAIN whole-file conversion, decode the output and compare every sample against the source. PASS proves the output is a bit-perfect replacement — safe to delete the source file. Processed conversions (rate/bit-depth changed) report a skip instead — keep the source."));
    m_deleteAfterVerifyCheck = new QCheckBox(tr("Delete source after PASS verify"), optBox);
    m_deleteAfterVerifyCheck->setChecked(false);
    m_deleteAfterVerifyCheck->setToolTip(tr("After a PASS bit-perfect verdict, permanently delete that job's source file — the automated form of the \"safe to delete the source\" note. Only an exact PASS ever deletes: skipped (processed) conversions, failures, and cancels always keep the source. Requires Verify output; the delete is permanent (not the recycle bin)."));
    // No proof → no delete: the option only exists while verify is on. Both
    // preferences persist (batch/verify, batch/deleteAfterVerify).
    m_deleteAfterVerifyCheck->setEnabled(m_verifyCheck->isChecked());
    connect(m_verifyCheck, &QCheckBox::toggled, this, [this](bool on) {
        QSettings s;
        s.setValue(QStringLiteral("batch/verify"), on);
        m_deleteAfterVerifyCheck->setEnabled(on);
    });
    connect(m_deleteAfterVerifyCheck, &QCheckBox::toggled, this, [this](bool on) {
        QSettings s;
        s.setValue(QStringLiteral("batch/deleteAfterVerify"), on);
    });
    optLay->addRow(QString(), m_parallelCheck);
    optLay->addRow(QString(), m_overwriteCheck);
    optLay->addRow(QString(), m_verifyCheck);
    optLay->addRow(QString(), m_deleteAfterVerifyCheck);
    lay->addWidget(optBox);

    // --- Process + progress + status ---
    m_processBtn = new QPushButton(tr("Process Queue"), this);
    m_processBtn->setEnabled(false);
    m_stopBtn = new QPushButton(tr("Stop"), this);
    m_stopBtn->setEnabled(false);
    m_stopBtn->setToolTip(tr("Stop the running batch: running jobs are cancelled, queued jobs are skipped."));
    auto* actionLay = new QHBoxLayout();
    actionLay->addWidget(m_processBtn, 1);
    actionLay->addWidget(m_stopBtn, 0);
    lay->addLayout(actionLay);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    lay->addWidget(m_progress);

    m_statusLabel = new QLabel(tr("Add capture files to the queue, then Process."), this);
    m_statusLabel->setWordWrap(true);
    lay->addWidget(m_statusLabel);
    lay->addStretch(1);

    connect(m_addBtn, &QPushButton::clicked, this, &BatchTab::addFilesDialog);
    connect(m_removeBtn, &QPushButton::clicked, this, &BatchTab::removeSelected);
    connect(m_clearBtn, &QPushButton::clicked, this, &BatchTab::clearQueue);
    connect(m_processBtn, &QPushButton::clicked, this, &BatchTab::processQueue);
    connect(m_stopBtn, &QPushButton::clicked, this, &BatchTab::stopProcess);
    connect(m_outDirBrowseBtn, &QPushButton::clicked, this, &BatchTab::browseOutDir);
    connect(m_outDirEdit, &QLineEdit::editingFinished, this, &BatchTab::onOutDirEdited);
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &BatchTab::onModeChanged);
    refreshQueueTable();
}

int BatchTab::addInputFiles(const QStringList& files)
{
    if (m_running)
        return 0;
    int added = 0;
    for (const QString& raw : files) {
        const QString in = normalizeInputPath(raw);
        if (in.isEmpty())
            continue;
        if (m_rowOfInput.contains(queuedFileKey(in)))
            continue;
        m_inputs.append(in);
        m_rowOfInput.insert(queuedFileKey(in), m_inputs.size() - 1);
        ++added;
    }
    if (added > 0) {
        refreshQueueTable();
        m_statusLabel->setText(added > 1
            ? tr("Added %1 files to the queue (total queued: %2).").arg(added).arg(m_inputs.size())
            : tr("Added 1 file to the queue (total queued: %1).").arg(m_inputs.size()));
        m_processBtn->setEnabled(true);
    }
    return added;
}

int BatchTab::automatedAddAndRun(const QStringList& files, bool autorun)
{
    // The CLI-->GUI load-test entry point (see MainWindow's
    // FLAC_CHOP_GUI_BATCH_FILES hook): the exact add + process path a user
    // drives by hand, callable from outside.
    if (m_running)
        return 0;
    const int added = addInputFiles(files);
    if (added > 0 && autorun && !m_running)
        processQueue();
    return added;
}

void BatchTab::setVerifyOutputOption(bool on)
{
    m_verifyCheck->setChecked(on);
}

void BatchTab::setDeleteAfterVerifyOption(bool on)
{
    m_deleteAfterVerifyCheck->setChecked(on);
}

void BatchTab::addFilesDialog()
{
    if (m_running)
        return;
    const QString startDir = m_inputs.isEmpty()
        ? QDir::homePath()
        : QFileInfo(m_inputs.last()).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add capture files to the queue"), startDir,
        tr("RF captures (*.flac *.ldf *.oga *.ogg *.wav *.u8 *.u16 *.s8 *.s16 *.r8 *.r16 *.8u *.8s *.16u *.16s *.raw *.bin *.pcm *.lds)"
           ";;FLAC / Ogg FLAC files (*.flac *.ldf *.oga *.ogg);;All files (*)"));
    if (files.isEmpty())
        return;
    const int added = addInputFiles(files);
    if (added == 0)
        m_statusLabel->setText(tr("No new files added (already queued)."));
}

void BatchTab::removeSelected()
{
    if (m_running || m_inputs.isEmpty())
        return;
    const QList<QTableWidgetItem*> selected = m_queue->selectedItems();
    if (selected.isEmpty()) {
        m_statusLabel->setText(tr("Select one or more queued files to remove."));
        return;
    }
    QSet<int> rows;
    for (const QTableWidgetItem* item : selected)
        rows.insert(item->row());
    QStringList surviving;
    for (int i = 0; i < m_inputs.size(); ++i)
        if (!rows.contains(i))
            surviving.append(m_inputs.at(i));
    m_inputs = surviving;
    refreshQueueTable();
    m_statusLabel->setText(tr("Removed %1 file(s) from the queue (remaining: %2).")
                               .arg(rows.size()).arg(m_inputs.size()));
}

void BatchTab::clearQueue()
{
    if (m_running || m_inputs.isEmpty())
        return;
    m_inputs.clear();
    refreshQueueTable();
    m_statusLabel->setText(tr("Cleared the input queue."));
}

void BatchTab::refreshQueueTable()
{
    m_rowOfInput.clear();
    m_queue->clearContents();
    m_queue->setRowCount(m_inputs.size());
    for (int row = 0; row < m_inputs.size(); ++row) {
        const QString& in = m_inputs.at(row);
        auto* pathItem = new QTableWidgetItem(QDir::toNativeSeparators(in));
        pathItem->setToolTip(in);
        m_queue->setItem(row, 0, pathItem);
        auto* statusItem = new QTableWidgetItem(tr("Queued"));
        statusItem->setToolTip(tr("Queued"));
        m_queue->setItem(row, 1, statusItem);
        m_rowOfInput.insert(queuedFileKey(in), row);
    }
    m_queueSummaryLabel->setText(m_inputs.isEmpty()
        ? tr("Queued files: none")
        : tr("Queued files: %1").arg(m_inputs.size()));
    m_processBtn->setEnabled(!m_inputs.isEmpty() && !m_running);
}

QString BatchTab::queuedFileKey(const QString& input) const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(input)).toLower();
}

int BatchTab::rowForInput(const QString& input) const
{
    return m_rowOfInput.value(queuedFileKey(input), -1);
}

void BatchTab::setRowStatus(int row, const QString& text, const QString& color)
{
    if (row < 0 || row >= m_queue->rowCount())
        return;
    QTableWidgetItem* item = m_queue->item(row, 1);
    if (!item)
        return;
    item->setText(text);
    item->setToolTip(text);
    if (!color.isEmpty())
        item->setForeground(QColor(color));
    else
        item->setForeground(palette().text());
}

void BatchTab::browseOutDir()
{
    const QString cur = m_outDirEdit->text().trimmed();
    const QString startDir = cur.isEmpty() ? QDir::homePath() : cur;
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select batch output directory"), startDir);
    if (d.isEmpty())
        return;
    m_outDirEdit->setText(d);
    refreshOutDirPlaceholder();
}

void BatchTab::onOutDirEdited()
{
    // Empty = "each input file's own folder" (no separate follow state: the
    // field's emptiness IS the state, same model as the Chop tab).
    refreshOutDirPlaceholder();
}

void BatchTab::onModeChanged()
{
    // The filter profile only applies with an explicit output rate.
    m_filterCheck->setEnabled(m_modeCombo->currentData().toULongLong() > 0);
}

void BatchTab::refreshOutDirPlaceholder()
{
    if (m_outDirEdit->text().trimmed().isEmpty())
        m_outDirEdit->setPlaceholderText(tr("(each input file's own folder)"));
}

void BatchTab::setControlsEnabled(bool enabled)
{
    m_addBtn->setEnabled(enabled);
    m_removeBtn->setEnabled(enabled);
    m_clearBtn->setEnabled(enabled);
    m_processBtn->setEnabled(enabled && !m_inputs.isEmpty());
    m_outDirEdit->setEnabled(enabled);
    m_outDirBrowseBtn->setEnabled(enabled);
    m_modeCombo->setEnabled(enabled);
    m_bitsCombo->setEnabled(enabled);
    m_filterCheck->setEnabled(enabled && m_modeCombo->currentData().toULongLong() > 0);
    m_parallelCheck->setEnabled(enabled);
    m_overwriteCheck->setEnabled(enabled);
    m_verifyCheck->setEnabled(enabled);
    m_deleteAfterVerifyCheck->setEnabled(enabled && m_verifyCheck->isChecked());
    m_queue->setEnabled(enabled);
}

void BatchTab::stopProcess()
{
    if (!m_running)
        return;
    m_stopRequested.store(true, std::memory_order_relaxed);
    // Cancel every job: running workers poll their own flag; jobs not yet
    // launched are skipped by the pump loop (their flag makes the worker
    // return immediately if the loop already handed it out).
    for (const auto& flag : m_cancelFlags) {
        if (flag)
            flag->store(1, std::memory_order_relaxed);
    }
    m_stopBtn->setEnabled(false);
    m_stopBtn->setText(tr("Stopping…"));
    m_statusLabel->setText(tr("Stopping…"));
}

void BatchTab::processQueue()
{
    if (m_running || m_inputs.isEmpty())
        return;

    const quint64 modeHz = m_modeCombo->currentData().toULongLong();
    const uint bits = m_bitsCombo->currentData().toUInt();
    const bool useFilter = (modeHz > 0) && m_filterCheck->isChecked();
    const bool overwrite = m_overwriteCheck->isChecked();
    const bool parallel = m_parallelCheck->isChecked();
    // Delete-after-verify only exists with proof: require BOTH checkboxes
    // (the delete box is disabled while verify is off, but a persisted
    // stale state or a rapid toggle must never enable a proof-less delete).
    const bool deleteAfterVerify = m_deleteAfterVerifyCheck->isChecked()
        && m_verifyCheck->isChecked();
    const QString outDir = m_outDirEdit->text().trimmed();

    // Pre-resolve every job on the GUI thread: deterministic skip-existing
    // checks and in-batch path uniqueness, with no disk races between the
    // parallel workers (lds-converter's pre-resolve model).
    struct Job {
        int row = -1;
        QString input;
        QString output;
        bool skip = false;
        QString skipReason;
    };
    // The queued inputs themselves are write-protected: a whole-file re-encode
    // of a MISRC-named capture (…_<B>-bit_<N>msps) with "keep source rate/bits"
    // degenerates to the input's OWN name — the fresh output path would be the
    // input itself (skip-existing would wrongly skip it; overwrite would try
    // to replace the input!). In that case the output takes the single-file
    // clobber-avoidance naming instead (…_8-bit_20msps-2.flac, exactly what
    // the Chop tab would write for the same request).
    QSet<QString> inputPathsLower;
    for (const QString& in : std::as_const(m_inputs))
        inputPathsLower.insert(in.toLower());

    QList<Job> jobs;
    // Seeded with the inputs so reserveUnique can never hand out an input's
    // path either.
    QSet<QString> reserved = inputPathsLower;
    for (const QString& in : std::as_const(m_inputs)) {
        Job job;
        job.row = rowForInput(in);
        job.input = in;
        const QString fresh = canonicalOutputPath(in, outDir, modeHz, bits);
        const bool targetsAnInput = inputPathsLower.contains(fresh.toLower());
        if (!overwrite && QFile::exists(fresh) && !targetsAnInput) {
            job.skip = true;
            job.skipReason = tr("Skipped — output already exists");
            jobs.append(job);
            continue;
        }
        if (targetsAnInput) {
            // Sidestep through the Rust clobber-avoidance naming (appends
            // -2, -3, … past every existing file, including the input).
            const QString stem = renamedOutputStem(in, modeHz, bits);
            char buf[4096];
            const QByteArray inB = in.toUtf8();
            const QByteArray dirB = outDir.toUtf8();
            const QByteArray stemB = stem.toUtf8();
            if (fc_generate_output_path(inB.constData(), dirB.constData(),
                                        stemB.constData(), buf, sizeof(buf))) {
                job.output = QString::fromUtf8(buf);
            } else {
                job.skip = true;
                job.skipReason = tr("Skipped — no usable output name");
                jobs.append(job);
                continue;
            }
        } else {
            job.output = fresh;
        }
        job.output = reserveUnique(job.output, reserved);
        jobs.append(job);
    }

    // Apply the skip statuses up front so they are visible during the run.
    for (const Job& job : std::as_const(jobs)) {
        if (job.skip)
            setRowStatus(job.row, job.skipReason, QStringLiteral("#a0a0a0"));
        else
            setRowStatus(job.row, tr("Queued"));
    }

    // The launch order (skip jobs are never launched).
    QList<int> launchOrder;
    for (int i = 0; i < jobs.size(); ++i)
        if (!jobs.at(i).skip)
            launchOrder.append(i);
    const int total = launchOrder.size();
    if (total == 0) {
        m_statusLabel->setText(tr("Nothing to process — every queued file was skipped."));
        return;
    }

    // One cancel flag per launchable job, alive for the whole run (the futures
    // are joined before processQueue returns, so the pointers stay valid).
    m_cancelFlags.clear();
    m_cancelFlags.reserve(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
        m_cancelFlags.emplace_back(std::make_unique<std::atomic<int>>(0));

    // Apply the bit-perfect verify preference before any job launches (the
    // core reads the global flag per chop).
    fc_set_verify_output(m_verifyCheck->isChecked() ? 1 : 0);

    m_running = true;
    const qint64 runStartMSecs = QDateTime::currentMSecsSinceEpoch();
    m_stopRequested.store(false, std::memory_order_relaxed);
    setControlsEnabled(false);
    m_stopBtn->setEnabled(true);
    m_stopBtn->setText(tr("Stop"));
    m_progress->setRange(0, total);
    m_progress->setValue(0);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    const int maxParallel = (parallel && total > 1)
        ? std::max(1, QThread::idealThreadCount())
        : 1;

    struct PendingTask {
        int jobIndex = -1;
        std::future<BatchResult> future;
    };
    std::vector<PendingTask> pending;
    std::vector<BatchResult> orderedResults(static_cast<size_t>(jobs.size()));
    std::vector<bool> resultReady(static_cast<size_t>(jobs.size()), false);
    int launched = 0;
    int completed = 0;

    while (completed < total && !(m_stopRequested.load(std::memory_order_relaxed) && pending.empty())) {
        // Fill the launch window (skipped when Stop was pressed).
        while (launched < total
               && static_cast<int>(pending.size()) < maxParallel
               && !m_stopRequested.load(std::memory_order_relaxed)) {
            const int jobIndex = launchOrder.at(launched);
            const Job job = jobs.at(jobIndex);
            const int slot = launched; // index into m_cancelFlags
            std::atomic<int>* flag = m_cancelFlags.at(static_cast<size_t>(slot)).get();
            m_statusLabel->setText(tr("Processing %1 of %2: %3")
                                        .arg(launched + 1).arg(total)
                                        .arg(QFileInfo(job.input).fileName()));
            setRowStatus(job.row, tr("Working…"));
            const quint64 mode = modeHz;
            const uint outBits = bits;
            const qint32 basic = useFilter ? 1 : 0;
            const bool doOverwrite = overwrite;
            pending.push_back(PendingTask{
                jobIndex,
                std::async(std::launch::async,
                           [this, job, jobIndex, mode, outBits, basic, doOverwrite,
                            deleteAfterVerify, flag]() -> BatchResult {
                               BatchResult res;
                               const ElapsedStamp stamp{
                                   QDateTime::currentMSecsSinceEpoch(),
                                   &res.elapsedMSecs};
                               // A stop that arrived before launch: never start.
                               if (flag->load(std::memory_order_relaxed) != 0) {
                                   res.cancelled = true;
                                   return res;
                               }
                               // Transient status ("Working…") via a queued
                               // connection — the FINAL status is applied on
                               // the GUI thread from the collected results.
                               BatchJobSignaller sig;
                               connect(&sig, &BatchJobSignaller::statusChanged,
                                       this,
                                       [this](int row, const QString& text) {
                                           setRowStatus(row, text);
                                       },
                                       Qt::QueuedConnection);
                               emit sig.statusChanged(job.row, QStringLiteral("Working…"));

                               FcProbe p{};
                               const QByteArray inB = job.input.toUtf8();
                               fc_probe(inB.constData(), &p);
                               if (!p.ok) {
                                   res.message = QString::fromUtf8(p.error);
                                   return res;
                               }
                               if (mode > 0) {
                                   if (!p.is_rf) {
                                       res.message = tr("output mode %1 needs an RF capture")
                                                         .arg(modeDisplay(mode));
                                       return res;
                                   }
                                   if (double(mode) * 1000.0 > p.real_rate_hz + 0.5) {
                                       res.message = tr("output mode %1 is above this file's rate (%2 Hz)")
                                                         .arg(modeDisplay(mode))
                                                         .arg(p.real_rate_hz, 0, 'f', 0);
                                       return res;
                                   }
                               }
                               if (!p.total_samples_known) {
                                   res.message = tr("unknown total sample count");
                                   return res;
                               }

                               // Whole-file job: start 0 → the probed total.
                               const quint64 len = p.total_samples;
                               const qint32 isRf = p.is_rf ? 1 : 0;

                               // Overwrite = replace an existing output (the
                               // skip-existing check ran against the canonical
                               // path; a reserved _2 path may still exist).
                               if (QFile::exists(job.output)) {
                                   if (!doOverwrite) {
                                       res.message = tr("output already exists: %1").arg(job.output);
                                       return res;
                                   }
                                   if (!QFile::remove(job.output)) {
                                       res.message = tr("could not replace the existing output: %1")
                                                         .arg(job.output);
                                       return res;
                                   }
                               }

                               FcChopResult r{};
                               const QByteArray outB = job.output.toUtf8();
                               fc_chop_ex(inB.constData(), outB.constData(), 0, len,
                                          mode, outBits, basic, isRf, &r,
                                          reinterpret_cast<const int32_t*>(flag));
                               if (r.ok) {
                                   res.ok = true;
                                   res.message = job.output;
                                   // Surface the bit-perfect verify verdict
                                   // (data-handling safety, the same note
                                   // the Chop tab shows).
                                   for (const QString& line :
                                        QString::fromUtf8(r.stderr_buf).split('\n')) {
                                       const QString t = line.trimmed();
                                       if (t.startsWith(QLatin1String("verify:")))
                                           res.verify = t;
                                   }
                                   // Opt-in delete-after-PASS: only an exact
                                   // PASS verdict (a proven bit-perfect
                                   // replacement) ever removes the source —
                                   // skips, failures, and cancels always keep
                                   // it. Belt-and-braces: never remove the
                                   // input itself (the pre-resolve already
                                   // re-routes outputs that would be an
                                   // input, but refuse regardless).
                                   if (deleteAfterVerify
                                       && res.verify.startsWith(QLatin1String("verify: PASS"))) {
                                       const QString inCanon =
                                           QFileInfo(job.input).absoluteFilePath();
                                       const QString outCanon =
                                           QFileInfo(job.output).absoluteFilePath();
                                       if (QString::compare(inCanon, outCanon,
                                                            Qt::CaseInsensitive) != 0
                                           && QFile::remove(job.input)) {
                                           res.deletedSource = true;
                                       }
                                   }
                                   return res;
                               }
                               const QString err = QString::fromUtf8(r.stderr_buf).trimmed();
                               const bool cancelled = flag->load(std::memory_order_relaxed) != 0
                                   || err.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive);
                               if (cancelled) {
                                   res.cancelled = true;
                                   // Never leave a partial output behind (a
                                   // later run would skip it as "exists").
                                   QFile::remove(job.output);
                                   return res;
                               }
                               // A failed job must not leave a corrupt/partial
                               // output either (same skip-existing reasoning).
                               QFile::remove(job.output);
                               res.message = err.isEmpty()
                                   ? tr("sox exit %1").arg(r.exit_code)
                                   : err;
                               return res;
                           })});
            ++launched;
        }

        // Collect finished futures (in flight order), update the counters.
        bool completedAny = false;
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                BatchResult res;
                try {
                    res = it->future.get();
                } catch (...) {
                    res.ok = false;
                    res.cancelled = m_stopRequested.load(std::memory_order_relaxed);
                    res.message = tr("job threw an exception");
                }
                orderedResults[static_cast<size_t>(it->jobIndex)] = res;
                resultReady[static_cast<size_t>(it->jobIndex)] = true;
                it = pending.erase(it);
                ++completed;
                completedAny = true;
                m_progress->setValue(completed);
                m_statusLabel->setText(tr("Batch progress: %1/%2 completed.").arg(completed).arg(total));
            } else {
                ++it;
            }
        }

        if (completed < total || !pending.empty())
            QCoreApplication::processEvents(QEventLoop::AllEvents,
                                            completedAny ? 0 : 50);
    }

    // Anything not launched or still pending was stopped: futures left in
    // `pending` here cannot exist (the loop only exits with it empty or the
    // stop case where we drain below). Guard anyway: join stragglers.
    for (auto& task : pending) {
        orderedResults[static_cast<size_t>(task.jobIndex)] = task.future.get();
        resultReady[static_cast<size_t>(task.jobIndex)] = true;
    }

    m_running = false;
    m_cancelFlags.clear();
    setControlsEnabled(true);
    m_stopBtn->setEnabled(false);
    m_stopBtn->setText(tr("Stop"));

    // Final per-row statuses + summary (queue order).
    int done = 0, skipped = 0, cancelledCount = 0;
    int verifiedPassed = 0, verifyFlagged = 0, deletedSources = 0;
    QStringList failed;
    for (int i = 0; i < jobs.size(); ++i) {
        const Job& job = jobs.at(i);
        if (job.skip) {
            ++skipped;
            continue;
        }
        if (!resultReady[static_cast<size_t>(i)]) {
            ++cancelledCount;
            setRowStatus(job.row, tr("Cancelled (not started)"), QStringLiteral("#e8a040"));
            continue;
        }
        const BatchResult& res = orderedResults[static_cast<size_t>(i)];
        if (res.ok) {
            ++done;
            if (res.verify.startsWith(QLatin1String("verify: PASS")))
                ++verifiedPassed;
            else if (!res.verify.isEmpty())
                ++verifyFlagged;
            if (res.deletedSource)
                ++deletedSources;
            QString text = tr("Done in %1 — %2")
                               .arg(formatElapsedMSecs(res.elapsedMSecs), res.message);
            if (!res.verify.isEmpty())
                text += QLatin1Char('\n') + res.verify;
            if (res.deletedSource)
                text += tr("\nsource deleted (verify PASS)");
            setRowStatus(job.row, text,
                         res.verify.startsWith(QLatin1String("verify: FAILED"))
                             ? QStringLiteral("#e8a040")
                             : QStringLiteral("#6fbf73"));
        } else if (res.cancelled) {
            ++cancelledCount;
            setRowStatus(job.row, tr("Cancelled (after %1)")
                             .arg(formatElapsedMSecs(res.elapsedMSecs)),
                         QStringLiteral("#e8a040"));
        } else {
            failed.append(job.input);
            setRowStatus(job.row, tr("Failed (after %1) — %2")
                             .arg(formatElapsedMSecs(res.elapsedMSecs), res.message),
                         QStringLiteral("#e06c60"));
        }
    }

    const bool stopped = m_stopRequested.load(std::memory_order_relaxed);
    const QString totalTime = formatElapsedMSecs(
        QDateTime::currentMSecsSinceEpoch() - runStartMSecs);
    QString summary = tr("Batch complete: %1 done, %2 skipped, %3 failed, %4 cancelled. Total time: %5.")
                          .arg(done).arg(skipped).arg(failed.size()).arg(cancelledCount)
                          .arg(totalTime);
    if (stopped)
        summary = tr("Batch stopped: %1 done, %2 skipped, %3 failed, %4 cancelled. Total time: %5.")
                      .arg(done).arg(skipped).arg(failed.size()).arg(cancelledCount)
                      .arg(totalTime);
    if (verifiedPassed > 0 || verifyFlagged > 0)
        summary += tr(" Verified: %1 passed, %2 flagged.")
                       .arg(verifiedPassed).arg(verifyFlagged);
    if (deletedSources > 0)
        summary += tr(" Deleted %1 source(s) after PASS.").arg(deletedSources);
    m_statusLabel->setText(summary);

    // Automation report (CLI-->GUI load testing): a machine-readable record
    // of this run for the parity harness. Written by ANY batch run (manual
    // clicks included) when FLAC_CHOP_GUI_TEST_OUT is set; setting
    // FLAC_CHOP_GUI_TEST_QUIT exits the app right after (automated runs
    // must terminate themselves).
    {
        const QByteArray reportPath = qgetenv("FLAC_CHOP_GUI_TEST_OUT");
        if (!reportPath.isEmpty()) {
            QJsonArray arr;
            for (int i = 0; i < jobs.size(); ++i) {
                const Job& job = jobs.at(i);
                QJsonObject o;
                o.insert(QStringLiteral("input"), job.input);
                o.insert(QStringLiteral("output"), job.output);
                o.insert(QStringLiteral("source_exists"), QFile::exists(job.input));
                if (job.skip) {
                    o.insert(QStringLiteral("status"), QStringLiteral("skipped"));
                    o.insert(QStringLiteral("skip_reason"), job.skipReason);
                } else if (!resultReady[static_cast<size_t>(i)]) {
                    o.insert(QStringLiteral("status"),
                             QStringLiteral("cancelled_not_started"));
                } else {
                    const BatchResult& res = orderedResults[static_cast<size_t>(i)];
                    o.insert(QStringLiteral("status"),
                             res.cancelled ? QStringLiteral("cancelled")
                                           : (res.ok ? QStringLiteral("ok")
                                                     : QStringLiteral("failed")));
                    o.insert(QStringLiteral("verify"), res.verify);
                    o.insert(QStringLiteral("deleted_source"), res.deletedSource);
                    o.insert(QStringLiteral("elapsed_ms"), double(res.elapsedMSecs));
                    if (!res.ok && !res.cancelled)
                        o.insert(QStringLiteral("error"), res.message);
                }
                arr.append(o);
            }
            QJsonObject root;
            root.insert(QStringLiteral("jobs"), arr);
            root.insert(QStringLiteral("summary"), summary);
            root.insert(QStringLiteral("deleted_sources"), deletedSources);
            QFile f(QString::fromLocal8Bit(reportPath));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            if (qEnvironmentVariableIsSet("FLAC_CHOP_GUI_TEST_QUIT"))
                QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
    }
    // The failure popup is interactive-only: an automated run (report set)
    // must never block on a modal box.
    if (!failed.isEmpty() && qgetenv("FLAC_CHOP_GUI_TEST_OUT").isEmpty())
        QMessageBox::warning(this, tr("Batch completed with errors"),
                             tr("Failed to process:\n%1")
                                 .arg(failed.join(QLatin1Char('\n'))));
}
