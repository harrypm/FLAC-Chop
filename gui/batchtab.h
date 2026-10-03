#ifndef FLACCHOP_BATCHTAB_H
#define FLACCHOP_BATCHTAB_H

// Batch Task tab — multi-file batch + parallel compression processing,
// ported from tbc-tools ld-lds-converter's ConverterDialog model:
//  - an input queue (Add… / Remove selected / Clear; duplicate-safe),
//  - every job runs whole-file (start 0 → the probed total) with the shared
//    output-processing settings (mode / bits / filter), like lds-converter
//    converts whole files,
//  - optional parallel processing: up to QThread::idealThreadCount()
//    concurrent jobs via std::async + futures with a sliding launch window
//    (a port of lds-converter's pump loop), every job on a worker thread so
//    the UI never freezes and Stop is always deliverable,
//  - per-job cancellation through a per-job std::atomic<int> flag handed to
//    fc_chop_ex (the legacy global fc_chop_cancel is single-cut only),
//  - per-row status + overall progress + a summary of done/failed/skipped.

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

// Per-job status reporter: created on the WORKER thread inside each job and
// connected to the tab with Qt::QueuedConnection (receiver = the tab on the
// GUI thread), so a job can report transient status ("Working…") safely while
// the pump loop keeps the UI alive. Final Done/Failed/Cancelled/Skipped
// statuses are applied from the collected results on the GUI thread, never
// through this signal (the object is dead once the job returns).
class BatchJobSignaller : public QObject {
    Q_OBJECT
public:
    explicit BatchJobSignaller(QObject* parent = nullptr) : QObject(parent) {}
signals:
    void statusChanged(int row, const QString& text);
};

class BatchTab : public QWidget {
    Q_OBJECT

public:
    explicit BatchTab(QWidget* parent = nullptr);

    // Append capture files to the queue (normalized + case-insensitive
    // dedup against what is already queued). Returns how many were newly
    // added. Refused while a batch run is in flight.
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
    void onModeChanged();

private:
    void buildUi();
    void refreshQueueTable();
    void refreshOutDirPlaceholder();
    void setRowStatus(int row, const QString& text, const QString& color = QString());
    int rowForInput(const QString& input) const;
    void setControlsEnabled(bool enabled);
    QString queuedFileKey(const QString& input) const;

    // --- queue state (GUI thread only) ---
    QStringList m_inputs;              // normalized, unique, in queue order
    QHash<QString, int> m_rowOfInput;   // queuedFileKey -> row

    // --- run state ---
    bool m_running = false;             // true while the pump loop is active
    std::atomic<bool> m_stopRequested{false};
    // One cancel flag per job, created before any launch and alive until
    // every future is joined — safe to hand raw pointers to the workers.
    std::vector<std::unique_ptr<std::atomic<int>>> m_cancelFlags;

    // --- widgets ---
    QTableWidget* m_queue = nullptr;
    QLabel* m_queueSummaryLabel = nullptr;
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

#endif // FLACCHOP_BATCHTAB_H
