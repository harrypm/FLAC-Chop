#include "syncedittab.h"

#include "stemutil.h"
#include "flacchop.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <utility>
#include <vector>

// Per-job outcome, collected from the futures in queue order.
namespace {
struct SyncResult {
    bool ok = false;
    bool cancelled = false;
    QString message;
    qint64 elapsedMSecs = 0; // wall-clock processing time of the job
};

QString normalizeInputPath(const QString& raw)
{
    QString t = raw.trimmed();
    if (t.isEmpty())
        return QString();
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(t));
    return QFileInfo(clean).absoluteFilePath();
}

// Canonical output path: <outdir-or-input-dir>/<renamed-stem>-sync.flac
// (the Batch tab naming, with a -sync suffix to distinguish Sync Edit
// outputs from batch outputs on the same files).
QString canonicalSyncOutputPath(const QString& input, const QString& outDir,
                                quint64 modeHz, uint bits)
{
    const QFileInfo info(input);
    QString stem = renamedOutputStem(input, modeHz, bits);
    if (stem.isEmpty())
        stem = info.completeBaseName() + QStringLiteral("-sync");
    else
        stem += QStringLiteral("-sync");
    const QString name = stem + QStringLiteral(".flac");
    const QString dir = outDir.isEmpty() ? info.absolutePath() : outDir;
    return QDir(dir).filePath(name);
}

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

const char* kFmtNames[] = { "FLAC", "WAV", "u8", "s8", "u16", "s16",
                            "Ogg FLAC", "lds" };

QString formatName(uint fmt)
{
    return (fmt < 8) ? QString::fromLatin1(kFmtNames[fmt]) : QStringLiteral("?");
}

QString secsToHms(double s)
{
    if (s < 0.0)
        s = 0.0;
    int whole = int(std::floor(s));
    int h = whole / 3600;
    int m = (whole % 3600) / 60;
    int sec = whole % 60;
    int ms = int(std::round((s - whole) * 1000.0));
    if (ms == 1000) { ms = 0; sec++; if (sec == 60) { sec = 0; m++; if (m == 60) { m = 0; h++; } } }
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(sec, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

// A single probe request + its result, moved across the future.
struct SyncProbeTask {
    QString path;
    FcProbe probe;
};
} // namespace

// --- Constructor -----------------------------------------------------------

SyncEditTab::SyncEditTab(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
    refreshOutDirPlaceholder();
    if (!fc_sox_available())
        m_statusLabel->setText(tr("WARNING: SoX not found (bundled or PATH) — processing will fail."));
}

// --- Queue management --------------------------------------------------------

int SyncEditTab::addInputFiles(const QStringList& files)
{
    if (m_running || m_probingCount > 0)
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
        m_statusLabel->setText(tr("Added %1 file(s). Probing…").arg(added));
        for (int row = m_inputs.size() - added; row < m_inputs.size(); ++row)
            startProbe(row);
    }
    return added;
}

void SyncEditTab::addFilesDialog()
{
    if (m_running || m_probingCount > 0)
        return;
    const QString startDir = m_inputs.isEmpty()
        ? QDir::homePath()
        : QFileInfo(m_inputs.last()).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add capture files to the set"), startDir,
        tr("RF captures (*.flac *.ldf *.oga *.ogg *.wav *.u8 *.u16 *.s8 *.s16 *.r8 *.r16 *.8u *.8s *.16u *.16s *.raw *.bin *.pcm *.lds)"
           ";;FLAC / Ogg FLAC files (*.flac *.ldf *.oga *.ogg);;All files (*)"));
    if (files.isEmpty())
        return;
    const int added = addInputFiles(files);
    if (added == 0)
        m_statusLabel->setText(tr("No new files added (already in the set)."));
}

void SyncEditTab::removeSelected()
{
    if (m_running || m_probingCount > 0 || m_inputs.isEmpty())
        return;
    const QList<QTableWidgetItem*> selected = m_queue->selectedItems();
    if (selected.isEmpty()) {
        m_statusLabel->setText(tr("Select one or more files to remove."));
        return;
    }
    QSet<int> rows;
    for (const QTableWidgetItem* item : selected)
        rows.insert(item->row());
    QStringList surviving;
    for (int i = 0; i < m_inputs.size(); ++i)
        if (!rows.contains(i))
            surviving.append(m_inputs.at(i));
    const int removed = m_inputs.size() - surviving.size();
    m_inputs = surviving;
    QSet<QString> keepKeys;
    for (const QString& in : surviving)
        keepKeys.insert(queuedFileKey(in));
    QSet<QString> toRemove;
    for (auto it = m_probeInfo.keyBegin(); it != m_probeInfo.keyEnd(); ++it)
        if (!keepKeys.contains(*it))
            toRemove.insert(*it);
    for (const QString& k : toRemove)
        m_probeInfo.remove(k);
    refreshQueueTable();
    refreshPreview();
    m_statusLabel->setText(tr("Removed %1 file(s) (remaining: %2).")
        .arg(removed).arg(m_inputs.size()));
}

void SyncEditTab::clearQueue()
{
    if (m_running || m_probingCount > 0 || m_inputs.isEmpty())
        return;
    m_inputs.clear();
    m_probeInfo.clear();
    refreshQueueTable();
    refreshPreview();
    m_statusLabel->setText(tr("Cleared the file set."));
}

// --- Probe (off-thread, the MainWindow model) --------------------------------

void SyncEditTab::startProbe(int row)
{
    if (row < 0 || row >= m_inputs.size())
        return;
    const QString key = queuedFileKey(m_inputs.at(row));
    m_probeRowOfKey.insert(key, row);
    ++m_probingCount;

    const QString path = m_inputs.at(row);
    auto* watcher = new QFutureWatcher<SyncProbeTask>(this);
    connect(watcher, &QFutureWatcher<SyncProbeTask>::finished,
            this, [this, watcher, key]() {
        watcher->deleteLater();
        const SyncProbeTask task = watcher->result();
        // Inline probe completion (avoids the extra slot indirection).
        m_probeRowOfKey.remove(key);
        --m_probingCount;
        SyncProbeInfo info;
        if (task.probe.ok) {
            info.ok = true;
            info.realRateHz = task.probe.real_rate_hz;
            info.isRf = task.probe.is_rf != 0;
            info.totalSamples = task.probe.total_samples;
            info.totalKnown = task.probe.total_samples_known != 0;
            info.totalSeconds = info.realRateHz > 0.0
                ? double(info.totalSamples) / info.realRateHz : 0.0;
            info.format = task.probe.format;
            info.bits = task.probe.bits_per_sample;
            info.channels = task.probe.channels;
        } else {
            info.ok = false;
            info.error = QString::fromUtf8(task.probe.error);
        }
        m_probeInfo.insert(key, info);

        const int r = m_rowOfInput.value(key, -1);
        if (r >= 0 && r < m_queue->rowCount()) {
            if (info.ok) {
                m_queue->item(r, 1)->setText(formatName(info.format));
                m_queue->item(r, 2)->setText(
                    info.isRf
                        ? QStringLiteral("%1 MSPS").arg(info.realRateHz / 1e6, 0, 'f', 1)
                        : QStringLiteral("%1 Hz").arg(info.realRateHz, 0, 'f', 0));
                m_queue->item(r, 3)->setText(info.totalKnown
                    ? secsToHms(info.totalSeconds) : tr("unknown"));
                setRowStatus(r, tr("Ready"), QStringLiteral("#2d8a4e"));
            } else {
                m_queue->item(r, 1)->setText(tr("error"));
                m_queue->item(r, 2)->setText(tr("—"));
                m_queue->item(r, 3)->setText(tr("—"));
                setRowStatus(r, tr("Probe failed"), QStringLiteral("#c0392b"));
            }
        }

        if (m_probingCount == 0) {
            refreshPreview();
            const int failed = std::count_if(m_probeInfo.cbegin(), m_probeInfo.cend(),
                                              [](const SyncProbeInfo& p) { return !p.ok; });
            m_statusLabel->setText(failed > 0
                ? tr("%1 of %2 probed OK (%3 failed).")
                    .arg(m_probeInfo.size() - failed).arg(m_probeInfo.size()).arg(failed)
                : tr("%1 file(s) probed. Set the time range, then Process.")
                    .arg(m_probeInfo.size()));
            m_processBtn->setEnabled(m_inputs.size() > 0 && failed < m_inputs.size());
        }
    });

    watcher->setFuture(QtConcurrent::run([path]() {
        SyncProbeTask task;
        task.path = path;
        fc_probe(path.toUtf8().constData(), &task.probe);
        return task;
    }));
}

void SyncEditTab::onProbeFinished()
{
    // Probes complete inline (lambda in startProbe); this slot is a
    // no-op kept for MOC compatibility with the header declaration.
}

// --- Time range ----------------------------------------------------------------

bool SyncEditTab::parseHms(const QString& s, double& outSec)
{
    QString t = s.trimmed();
    if (t.isEmpty()) {
        outSec = 0.0;
        return true;    // empty = 0
    }
    const QStringList parts = t.split(QLatin1Char(':'));
    if (parts.size() > 3)
        return false;
    bool ok = false;
    double h = 0.0, m = 0.0, sec = 0.0;
    if (parts.size() == 3) {
        h = parts[0].toDouble(&ok);
        if (!ok || h < 0.0) return false;
        m = parts[1].toDouble(&ok);
        if (!ok || m < 0.0 || m >= 60.0) return false;
        sec = parts[2].toDouble(&ok);
        if (!ok || sec < 0.0 || sec >= 60.0) return false;
    } else if (parts.size() == 2) {
        m = parts[0].toDouble(&ok);
        if (!ok || m < 0.0) return false;
        sec = parts[1].toDouble(&ok);
        if (!ok || sec < 0.0 || sec >= 60.0) return false;
    } else {
        sec = parts[0].toDouble(&ok);
        if (!ok || sec < 0.0) return false;
    }
    outSec = h * 3600.0 + m * 60.0 + sec;
    return true;
}

void SyncEditTab::onTimeRangeChanged()
{
    refreshPreview();
}

void SyncEditTab::refreshPreview()
{
    if (!m_previewLabel)
        return;
    double inSec = 0.0, outSec = 0.0;
    const bool inOk = parseHms(m_inEdit->text(), inSec);
    const bool outOk = parseHms(m_outEdit->text(), outSec);
    if (!inOk || !outOk) {
        m_previewLabel->setText(tr("Invalid time — use HH:MM:SS, MM:SS, or SS.mmm"));
        m_processBtn->setEnabled(false);
        return;
    }
    const bool toEnd = (outSec <= 0.0);
    const double cutLen = toEnd ? -1.0 : (outSec - inSec);
    if (!toEnd && cutLen <= 0.0) {
        m_previewLabel->setText(tr("OUT must be after IN (or 0/empty = to the end)."));
        m_processBtn->setEnabled(false);
        return;
    }

    QStringList lines;
    int readyCount = 0;
    for (int row = 0; row < m_inputs.size(); ++row) {
        const QString key = queuedFileKey(m_inputs.at(row));
        const SyncProbeInfo& info = m_probeInfo.value(key);
        if (!info.ok || info.realRateHz <= 0.0)
            continue;
        ++readyCount;
        const quint64 startSamp = quint64(std::round(inSec * info.realRateHz));
        quint64 lenSamp;
        if (toEnd) {
            lenSamp = info.totalSamples > startSamp
                ? info.totalSamples - startSamp : 0;
        } else {
            lenSamp = quint64(std::round(cutLen * info.realRateHz));
            if (info.totalKnown && startSamp + lenSamp > info.totalSamples)
                lenSamp = info.totalSamples - startSamp;
        }
        const QFileInfo fi(m_inputs.at(row));
        lines.append(QStringLiteral("%1: %2 → %3 (%4 samples)")
            .arg(fi.fileName())
            .arg(secsToHms(inSec))
            .arg(secsToHms(toEnd ? info.totalSeconds : outSec))
            .arg(lenSamp));
    }
    if (readyCount == 0) {
        m_previewLabel->setText(tr("Add files to the set to see the cut preview."));
        m_processBtn->setEnabled(false);
        return;
    }
    m_previewLabel->setText(lines.join(QLatin1Char('\n')));
    m_processBtn->setEnabled(m_probingCount == 0);
}

// --- Process --------------------------------------------------------------------

void SyncEditTab::processQueue()
{
    if (m_running || m_inputs.isEmpty())
        return;
    double inSec = 0.0, outSec = 0.0;
    if (!parseHms(m_inEdit->text(), inSec) || !parseHms(m_outEdit->text(), outSec)) {
        QMessageBox::warning(this, tr("Sync Edit"), tr("Invalid IN or OUT time."));
        return;
    }
    const bool toEnd = (outSec <= 0.0);
    const double cutLen = toEnd ? -1.0 : (outSec - inSec);
    if (!toEnd && cutLen <= 0.0) {
        QMessageBox::warning(this, tr("Sync Edit"),
            tr("OUT must be after IN (or 0/empty to cut to the end of each file)."));
        return;
    }

    // Collect the per-file jobs (probe-derived sample counts).
    struct SyncJob {
        QString input;
        QString output;
        quint64 startSamples;
        quint64 lengthSamples;
        SyncProbeInfo info;
        int row;
    };
    std::vector<SyncJob> jobs;
    QSet<QString> reserved;
    const quint64 modeHz = m_modeCombo->currentData().toULongLong();
    const uint bits = m_bitsCombo->currentData().toUInt();
    const bool filter = m_filterCheck->isChecked();
    const bool overwrite = m_overwriteCheck->isChecked();
    const QString outDir = m_outDirEdit->text().trimmed();

    for (int row = 0; row < m_inputs.size(); ++row) {
        const QString& input = m_inputs.at(row);
        const QString key = queuedFileKey(input);
        const SyncProbeInfo info = m_probeInfo.value(key);
        if (!info.ok || info.realRateHz <= 0.0)
            continue;

        const quint64 startSamp = quint64(std::round(inSec * info.realRateHz));
        quint64 lenSamp;
        if (toEnd) {
            lenSamp = info.totalSamples > startSamp
                ? info.totalSamples - startSamp : 0;
        } else {
            lenSamp = quint64(std::round(cutLen * info.realRateHz));
            if (info.totalKnown && startSamp + lenSamp > info.totalSamples)
                lenSamp = info.totalSamples - startSamp;
        }
        if (lenSamp == 0)
            continue;

        SyncJob job;
        job.input = input;
        job.output = reserveUnique(
            canonicalSyncOutputPath(input, outDir, modeHz, bits), reserved);
        job.startSamples = startSamp;
        job.lengthSamples = lenSamp;
        job.info = info;
        job.row = row;
        jobs.push_back(std::move(job));
    }

    if (jobs.empty()) {
        m_statusLabel->setText(tr("Nothing to process — check the time range."));
        return;
    }

    // Skip-existing / overwrite.
    if (!overwrite) {
        std::vector<SyncJob> run;
        int skipped = 0;
        for (auto& j : jobs) {
            if (QFile::exists(j.output)) {
                setRowStatus(j.row, tr("Skipped — output already exists"),
                            QStringLiteral("#b8860b"));
                ++skipped;
            } else {
                run.push_back(std::move(j));
            }
        }
        if (run.empty()) {
            m_statusLabel->setText(tr("All outputs already exist — nothing to do (tick Overwrite to replace)."));
            return;
        }
        if (skipped > 0)
            m_statusLabel->setText(tr("%1 file(s) skipped (output exists). Processing %2…")
                .arg(skipped).arg(run.size()));
        jobs = std::move(run);
    }

    // Never write onto a queued input.
    for (auto& j : jobs) {
        const QString jKey = queuedFileKey(j.output);
        if (m_rowOfInput.contains(jKey)) {
            const QFileInfo info(j.output);
            for (int n = 2;; ++n) {
                const QString cand = QDir(info.absolutePath()).filePath(
                    QStringLiteral("%1-%2.flac")
                        .arg(info.completeBaseName()).arg(n));
                if (!m_rowOfInput.contains(queuedFileKey(cand))
                    && !QFile::exists(cand)) {
                    j.output = cand;
                    break;
                }
            }
        }
    }

    // --- Run (the Batch tab's sliding-window pump loop model) ---
    m_running = true;
    const qint64 runStartMSecs = QDateTime::currentMSecsSinceEpoch();
    m_stopRequested.store(false);
    setControlsEnabled(false);
    m_stopBtn->setEnabled(true);
    m_progress->setRange(0, int(jobs.size()));
    m_progress->setValue(0);

    const int maxThreads = m_parallelCheck->isChecked()
        ? std::max(1, QThread::idealThreadCount()) : 1;
    const int totalJobs = int(jobs.size());

    m_cancelFlags.clear();
    m_cancelFlags.resize(jobs.size());
    for (auto& f : m_cancelFlags)
        f = std::make_unique<std::atomic<int>>(0);

    std::vector<std::future<SyncResult>> futures;
    futures.reserve(jobs.size());

    for (size_t i = 0; i < jobs.size(); ++i) {
        const auto& job = jobs[i];
        auto* cancelFlag = m_cancelFlags[i].get();
        const int row = job.row;

        const quint64 outRate = modeHz;
        const uint outBits = bits;
        const bool doFilter = filter;
        const qint32 basic = (outRate > 0 && doFilter && job.info.isRf) ? 1 : 0;
        const qint32 isRf = job.info.isRf ? 1 : 0;
        const QString inPath = job.input;
        const QString outPath = job.output;
        const quint64 startSamp = job.startSamples;
        const quint64 lenSamp = job.lengthSamples;

        futures.push_back(std::async(std::launch::async,
            [inPath, outPath, startSamp, lenSamp, outRate, outBits, basic,
             isRf, cancelFlag]() -> SyncResult {
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            FcChopResult r{};
            fc_chop_ex(inPath.toUtf8().constData(), outPath.toUtf8().constData(),
                        startSamp, lenSamp, outRate, outBits, basic, isRf,
                        &r, reinterpret_cast<const int32_t*>(cancelFlag));
            SyncResult res;
            res.ok = r.ok != 0;
            res.message = QString::fromUtf8(r.stderr_buf);
            res.elapsedMSecs = QDateTime::currentMSecsSinceEpoch() - t0;
            if (cancelFlag->load() != 0)
                res.cancelled = true;
            return res;
        }));
    }

    // Sliding-window pump: join futures as they complete, via a zero-timeout
    // timer so the GUI thread stays alive (Stop works, the table updates).
    auto* pumpTimer = new QTimer(this);
    pumpTimer->setInterval(0);
    int joined = 0, done = 0, failed = 0, cancelledCount = 0;
    connect(pumpTimer, &QTimer::timeout, this, [this, pumpTimer, &futures,
            &joined, &done, &failed, &cancelledCount, totalJobs, &jobs,
            &runStartMSecs]() {
        // Stop requested: cancel any not-yet-joined jobs.
        if (m_stopRequested.load()) {
            for (size_t i = joined; i < m_cancelFlags.size(); ++i)
                m_cancelFlags[i]->store(1);
        }
        // Join any that are ready (non-blocking, check the oldest).
        while (joined < totalJobs) {
            auto& fut = futures[joined];
            if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                break;
            const auto& job = jobs[joined];
            const SyncResult res = fut.get();
            if (res.cancelled) {
                setRowStatus(job.row, tr("Cancelled (after %1)")
                                 .arg(formatElapsedMSecs(res.elapsedMSecs)),
                             QStringLiteral("#b8860b"));
                ++cancelledCount;
            } else if (res.ok) {
                setRowStatus(job.row, tr("Done in %1 — %2").arg(
                    formatElapsedMSecs(res.elapsedMSecs),
                    QDir::toNativeSeparators(job.output)),
                    QStringLiteral("#2d8a4e"));
                ++done;
            } else {
                setRowStatus(job.row, tr("Failed (after %1) — %2").arg(
                    formatElapsedMSecs(res.elapsedMSecs), res.message),
                    QStringLiteral("#c0392b"));
                ++failed;
            }
            ++joined;
            m_progress->setValue(joined);
        }
        // All done?
        if (joined >= totalJobs) {
            pumpTimer->stop();
            pumpTimer->deleteLater();
            m_running = false;
            m_stopRequested.store(false);
            m_stopBtn->setEnabled(false);
            setControlsEnabled(true);
            m_statusLabel->setText(tr("Sync edit done: %1 ok, %2 failed, %3 cancelled. Total time: %4.")
                .arg(done).arg(failed).arg(cancelledCount)
                .arg(formatElapsedMSecs(
                    QDateTime::currentMSecsSinceEpoch() - runStartMSecs)));
        }
    });
    pumpTimer->start();
    if (!m_statusLabel->text().contains("skipped"))
        m_statusLabel->setText(tr("Processing %1 file(s)…").arg(jobs.size()));
}

void SyncEditTab::stopProcess()
{
    if (!m_running)
        return;
    m_stopRequested.store(true);
    m_statusLabel->setText(tr("Stopping — running jobs are cancelled…"));
    for (auto& f : m_cancelFlags)
        f->store(1);
}

// --- UI ------------------------------------------------------------------------

void SyncEditTab::buildUi()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    // --- File set queue ---
    auto* queueBox = new QGroupBox(tr("File Set (files from the same capture session / tape)"), this);
    auto* queueLay = new QVBoxLayout(queueBox);
    m_queue = new QTableWidget(0, 5, queueBox);
    m_queue->setHorizontalHeaderLabels({tr("File"), tr("Format"), tr("Rate"),
                                        tr("Duration"), tr("Status")});
    m_queue->verticalHeader()->setVisible(false);
    m_queue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queue->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queue->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c)
        m_queue->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_queue->setToolTip(tr(
        "Files from the same capture session (e.g. 2x RF + 1-5x audio). "
        "All files cover the same tape time. The Sync Edit cut is applied "
        "to every file at the same TAPE position, using each file's own "
        "probed real rate to convert the time range to sample counts."));
    queueLay->addWidget(m_queue);

    auto* queueBtnRow = new QHBoxLayout();
    m_addBtn = new QPushButton(tr("Add…"), queueBox);
    m_removeBtn = new QPushButton(tr("Remove selected"), queueBox);
    m_clearBtn = new QPushButton(tr("Clear"), queueBox);
    m_addBtn->setToolTip(tr("Add capture files to the set (multi-select)."));
    m_removeBtn->setToolTip(tr("Remove the selected rows from the set."));
    m_clearBtn->setToolTip(tr("Clear the whole set."));
    m_queueSummaryLabel = new QLabel(tr("Files in set: 0"), queueBox);
    queueBtnRow->addWidget(m_addBtn);
    queueBtnRow->addWidget(m_removeBtn);
    queueBtnRow->addWidget(m_clearBtn);
    queueBtnRow->addStretch(1);
    queueBtnRow->addWidget(m_queueSummaryLabel);
    queueLay->addLayout(queueBtnRow);
    lay->addWidget(queueBox);

    // --- Sync time range (the ONE edit applied to every file) ---
    auto* rangeBox = new QGroupBox(tr("Sync Time Range (applied to every file at the same tape position)"), this);
    auto* rangeLay = new QFormLayout(rangeBox);
    m_inEdit = new QLineEdit(QStringLiteral("00:00:00"), rangeBox);
    m_inEdit->setToolTip(tr(
        "Where to START keeping content (real tape time). Everything before "
        "this is removed from every file. HH:MM:SS, MM:SS, or SS.mmm."));
    m_outEdit = new QLineEdit(rangeBox);
    m_outEdit->setToolTip(tr(
        "Where to STOP keeping content (real tape time). 0 or empty = keep "
        "to the end of each file. Everything after this is removed."));
    m_previewLabel = new QLabel(rangeBox);
    m_previewLabel->setWordWrap(true);
    rangeLay->addRow(tr("IN (start keeping):"), m_inEdit);
    rangeLay->addRow(tr("OUT (stop keeping):"), m_outEdit);
    rangeLay->addRow(tr("Preview:"), m_previewLabel);
    lay->addWidget(rangeBox);

    // --- Output directory ---
    auto* outDirBox = new QGroupBox(tr("Output Directory"), this);
    auto* outDirLay = new QHBoxLayout(outDirBox);
    m_outDirEdit = new QLineEdit(outDirBox);
    m_outDirEdit->setToolTip(tr("Outputs are written here. Empty = each file's own folder."));
    m_outDirBrowseBtn = new QPushButton(tr("Browse..."), outDirBox);
    outDirLay->addWidget(m_outDirEdit, 1);
    outDirLay->addWidget(m_outDirBrowseBtn, 0);
    lay->addWidget(outDirBox);

    // --- Output processing ---
    auto* setBox = new QGroupBox(tr("Output Processing (applied to every file)"), this);
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
    auto* optBox = new QGroupBox(tr("Sync Edit Options"), this);
    auto* optLay = new QFormLayout(optBox);
    m_parallelCheck = new QCheckBox(tr("Parallel processing"), optBox);
    m_parallelCheck->setChecked(true);
    m_parallelCheck->setToolTip(tr("Run file cuts concurrently, up to %1 jobs at once. Every job runs on a worker thread either way, so Stop always works.")
        .arg(std::max(1, QThread::idealThreadCount())));
    m_overwriteCheck = new QCheckBox(tr("Overwrite existing outputs"), optBox);
    m_overwriteCheck->setToolTip(tr("Unchecked: a file whose output already exists is skipped. Checked: the existing output is replaced."));
    optLay->addRow(QString(), m_parallelCheck);
    optLay->addRow(QString(), m_overwriteCheck);
    lay->addWidget(optBox);

    // --- Process + progress + status ---
    m_processBtn = new QPushButton(tr("Process Sync Edit"), this);
    m_processBtn->setEnabled(false);
    m_stopBtn = new QPushButton(tr("Stop"), this);
    m_stopBtn->setEnabled(false);
    m_stopBtn->setToolTip(tr("Stop the running sync edit: running jobs are cancelled, not-yet-started are skipped."));
    auto* actionLay = new QHBoxLayout();
    actionLay->addWidget(m_processBtn, 1);
    actionLay->addWidget(m_stopBtn, 0);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_statusLabel = new QLabel(tr("Add a file set from the same capture session (RF + audio files), then set the time range."), this);
    m_statusLabel->setWordWrap(true);
    lay->addLayout(actionLay);
    lay->addWidget(m_progress);
    lay->addWidget(m_statusLabel);
    lay->addStretch(1);

    // --- Connect ---
    connect(m_addBtn, &QPushButton::clicked, this, &SyncEditTab::addFilesDialog);
    connect(m_removeBtn, &QPushButton::clicked, this, &SyncEditTab::removeSelected);
    connect(m_clearBtn, &QPushButton::clicked, this, &SyncEditTab::clearQueue);
    connect(m_processBtn, &QPushButton::clicked, this, &SyncEditTab::processQueue);
    connect(m_stopBtn, &QPushButton::clicked, this, &SyncEditTab::stopProcess);
    connect(m_outDirBrowseBtn, &QPushButton::clicked, this, &SyncEditTab::browseOutDir);
    connect(m_outDirEdit, &QLineEdit::editingFinished, this, &SyncEditTab::onOutDirEdited);
    connect(m_inEdit, &QLineEdit::textChanged, this, &SyncEditTab::onTimeRangeChanged);
    connect(m_outEdit, &QLineEdit::textChanged, this, &SyncEditTab::onTimeRangeChanged);
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SyncEditTab::onModeChanged);
}

void SyncEditTab::refreshQueueTable()
{
    m_rowOfInput.clear();
    m_queue->clearContents();
    m_queue->setRowCount(m_inputs.size());
    for (int row = 0; row < m_inputs.size(); ++row) {
        const QString& in = m_inputs.at(row);
        auto* pathItem = new QTableWidgetItem(QDir::toNativeSeparators(in));
        pathItem->setToolTip(in);
        m_queue->setItem(row, 0, pathItem);
        m_queue->setItem(row, 1, new QTableWidgetItem(tr("probing…")));
        m_queue->setItem(row, 2, new QTableWidgetItem(tr("—")));
        m_queue->setItem(row, 3, new QTableWidgetItem(tr("—")));
        auto* statusItem = new QTableWidgetItem(tr("Probing…"));
        statusItem->setToolTip(tr("Probing…"));
        m_queue->setItem(row, 4, statusItem);
        m_rowOfInput.insert(queuedFileKey(in), row);
    }
    m_queueSummaryLabel->setText(m_inputs.isEmpty()
        ? tr("Files in set: none")
        : tr("Files in set: %1").arg(m_inputs.size()));
    m_processBtn->setEnabled(!m_inputs.isEmpty() && m_probingCount == 0);
}

void SyncEditTab::setRowStatus(int row, const QString& text, const QString& color)
{
    if (row < 0 || row >= m_queue->rowCount())
        return;
    QTableWidgetItem* item = m_queue->item(row, 4);
    if (!item)
        return;
    item->setText(text);
    item->setToolTip(text);
    if (!color.isEmpty())
        item->setForeground(QColor(color));
    else
        item->setForeground(palette().text());
}

int SyncEditTab::rowForInput(const QString& input) const
{
    return m_rowOfInput.value(queuedFileKey(input), -1);
}

QString SyncEditTab::queuedFileKey(const QString& input) const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(input)).toLower();
}

void SyncEditTab::setControlsEnabled(bool enabled)
{
    m_addBtn->setEnabled(enabled);
    m_removeBtn->setEnabled(enabled);
    m_clearBtn->setEnabled(enabled);
    m_processBtn->setEnabled(enabled && !m_inputs.isEmpty());
    m_outDirEdit->setEnabled(enabled);
    m_outDirBrowseBtn->setEnabled(enabled);
    m_modeCombo->setEnabled(enabled);
    m_bitsCombo->setEnabled(enabled);
    m_filterCheck->setEnabled(enabled);
    m_inEdit->setEnabled(enabled);
    m_outEdit->setEnabled(enabled);
}

void SyncEditTab::browseOutDir()
{
    const QString cur = m_outDirEdit->text().trimmed();
    const QString startDir = cur.isEmpty() ? QDir::homePath() : cur;
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select output directory"), startDir);
    if (d.isEmpty())
        return;
    m_outDirEdit->setText(d);
    refreshOutDirPlaceholder();
}

void SyncEditTab::onOutDirEdited()
{
    refreshOutDirPlaceholder();
}

void SyncEditTab::refreshOutDirPlaceholder()
{
    if (!m_outDirEdit)
        return;
    if (!m_outDirEdit->text().trimmed().isEmpty())
        return;
    m_outDirEdit->setPlaceholderText(tr("(follows input: each file's own folder)"));
}

void SyncEditTab::onModeChanged()
{
    const quint64 outRate = m_modeCombo->currentData().toULongLong();
    m_filterCheck->setEnabled(outRate > 0);
}
