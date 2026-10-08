#ifndef FLACCHOP_SYNCEDITTAB_H
#define FLACCHOP_SYNCEDITTAB_H

// Sync Edit tab — synchronized time-range cuts across a file set.
//
// Use case: a tape capture session produces a SET of files (e.g. 2x RF +
// 1-5x audio) that all cover the SAME tape time. Removing dead space
// (blank tape at the start/end) from every file individually is tedious;
// this tab applies ONE time range (IN/OUT, real tape seconds) to every
// file in the set at once — each file gets its own sample counts derived
// from its own probed real rate, so the cuts land at the same tape
// position in every file.
//
// Model:
//  - A queue of files (Add… / Remove / Clear; duplicate-safe; probed
//    on add so the table shows format / rate / duration per file).
//  - ONE time range: IN (start keeping) and OUT (stop keeping; 0/empty =
//    to the end of each file), in real tape seconds (HH:MM:SS or decimal).
//  - A live preview of the per-file cut plan (each file's start/length
//    samples at its own rate) that updates as the time range changes.
//  - Optional output settings (rate / bits / filter, like the Batch tab).
//  - Process: cuts every file at the same tape time range, using each
//    file's own probed rate to convert the time range to sample counts.
//  - Parallel processing + per-job cancel (the Batch tab model).

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QProgressBar;

// Per-job probe result (stored per queued file for the preview + cut).
struct SyncProbeInfo {
    bool ok = false;
    QString error;
    double realRateHz = 0.0;    // the file's resolved real rate
    bool isRf = false;
    quint64 totalSamples = 0;
    bool totalKnown = false;
    double totalSeconds = 0.0;   // total / realRateHz
    uint format = 0;            // FcProbe format code
    uint bits = 0;
    uint channels = 0;
};

// Per-job status reporter (the Batch tab model).
class SyncJobSignaller : public QObject {
    Q_OBJECT
public:
    explicit SyncJobSignaller(QObject* parent = nullptr) : QObject(parent) {}
signals:
    void statusChanged(int row, const QString& text);
};

class SyncEditTab : public QWidget {
    Q_OBJECT

public:
    explicit SyncEditTab(QWidget* parent = nullptr);

    // Append capture files to the set (normalized + dedup). Returns how
    // many were newly added. Refused while a run is in flight.
    int addInputFiles(const QStringList& files);

    bool isBusy() const { return m_running; }

private slots:
    void addFilesDialog();
    void removeSelected();
    void clearQueue();
    void processQueue();
    void stopProcess();
    void browseOutDir();
    void onOutDirEdited();
    void onTimeRangeChanged();
    void onModeChanged();
    void onProbeFinished();

private:
    void buildUi();
    void refreshQueueTable();
    void refreshOutDirPlaceholder();
    void setRowStatus(int row, const QString& text, const QString& color = QString());
    int rowForInput(const QString& input) const;
    void setControlsEnabled(bool enabled);
    QString queuedFileKey(const QString& input) const;
    void startProbe(int row);
    void refreshPreview();
    static bool parseHms(const QString& s, double& outSec);

    // --- queue state (GUI thread only) ---
    QStringList m_inputs;                 // normalized, unique, in queue order
    QHash<QString, int> m_rowOfInput;    // queuedFileKey -> row
    QHash<QString, SyncProbeInfo> m_probeInfo; // queuedFileKey -> probe result

    // --- probe state ---
    int m_probingCount = 0;              // > 0 while probes are in flight
    QHash<QString, int> m_probeRowOfKey; // key -> row (for probe callback)

    // --- run state ---
    bool m_running = false;
    std::atomic<bool> m_stopRequested{false};
    std::vector<std::unique_ptr<std::atomic<int>>> m_cancelFlags;

    // --- widgets ---
    QTableWidget* m_queue = nullptr;
    QLabel* m_queueSummaryLabel = nullptr;
    QLineEdit* m_inEdit = nullptr;       // IN time (HH:MM:SS or decimal seconds)
    QLineEdit* m_outEdit = nullptr;     // OUT time (0/empty = to the end)
    QLabel* m_inLabel = nullptr;        // live IN display (seconds)
    QLabel* m_outLabel = nullptr;       // live OUT display (seconds)
    QLabel* m_previewLabel = nullptr;    // per-file cut preview
    QLineEdit* m_outDirEdit = nullptr;
    QPushButton* m_outDirBrowseBtn = nullptr;
    QComboBox* m_modeCombo = nullptr;
    QComboBox* m_bitsCombo = nullptr;
    QCheckBox* m_filterCheck = nullptr;
    QCheckBox* m_parallelCheck = nullptr;
    QCheckBox* m_overwriteCheck = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QPushButton* m_processBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_statusLabel = nullptr;
};

#endif // FLACCHOP_SYNCEDITTAB_H
