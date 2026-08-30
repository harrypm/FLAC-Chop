#ifndef FLACCHOP_MAINWINDOW_H
#define FLACCHOP_MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QFutureWatcher>

#include "flacchop.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QRangeSlider;
class QComboBox;
class QCheckBox;
class QNetworkAccessManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void browse();
    void process();
    void onProbeFinished();
    void onChopFinished();
    void onSliderInChanged(int v);
    void onSliderOutChanged(int v);
    void setInFromBox();
    void setOutFromBox();
    void checkForUpdatesManual();
    void browseOutDir();
    void onOutDirEdited();
    void cancelProcess();

private:
    // HH:MM:SS parsing helpers (accept "SS", "MM:SS", "HH:MM:SS").
    static bool parseHms(const QString& s, double& outSec);
    static QString secsToHms(double s);
    void loadFile(const QString& fn);
    void unloadFile();
    void setProbeInfo();
    void setControlsEnabled(bool enabled);
    // Apply m_inSec/m_outSec to the cut plan + read-only displays. Does NOT
    // touch the slider or the time box (callers do that with signals blocked).
    void applyCut();
    // Push m_inSec/m_outSec into the slider handles (signals blocked).
    void syncSliderFromCut();
    // Set the time box text (signals blocked) to a given seconds value.
    void setTimeBox(double sec);
    void maybeCheckForUpdates();
    void checkForUpdates(bool manual);
    // Effective output directory for cuts: the user-chosen dir if non-empty,
    // else the input file's directory (the original sibling -cut.flac
    // behaviour). Persisted in QSettings("output/dir").
    QString effectiveOutDir() const;
    void persistOutDir(const QString& dir);
    // Rename the output stem to reflect the new altered metadata
    // (e.g. 20msps_8-bit -> 16msps_6-bit) when the input name matches the
    // MISRC `<N>msps_<B>-bit` convention; else empty (stock <stem>-cut).
    QString renamedOutputStem() const;

    QString m_inPath;
    FcProbe m_probe{};
    bool m_probeOk = false;
    int m_sliderMaxDs = 0;    // slider range in deciseconds (0.1 s)
    double m_totalSec = 0.0;  // real total duration (s), 0 if unknown

    // Source of truth for the cut, in real seconds. Mutated only by
    // setInFromBox / setOutFromBox / onSliderInChanged / onSliderOutChanged /
    // loadFile. Never bound to a textChanged signal, so loading can't be
    // clobbered by a stray recompute.
    double m_inSec = 0.0;
    double m_outSec = 0.0;

    // owned widgets
    QLineEdit* m_pathLabel = nullptr;   // input file path (read-only, in-lay)
    QPushButton* m_browseBtn = nullptr;
    QLineEdit* m_timeEdit = nullptr;   // the single editable time box
    QPushButton* m_setInBtn = nullptr;
    QPushButton* m_setOutBtn = nullptr;
    QLabel* m_inLabel = nullptr;       // read-only IN display
    QLabel* m_outLabel = nullptr;      // read-only OUT display
    QLabel* m_durLabel = nullptr;      // read-only Duration display
    QRangeSlider* m_slider = nullptr;
    QLabel* m_headerRateLabel = nullptr;
    QLabel* m_bitsChLabel = nullptr;
    QLabel* m_mspsLabel = nullptr;
    QLabel* m_totalLabel = nullptr;
    QLabel* m_startSampLabel = nullptr;
    QLabel* m_lenSampLabel = nullptr;
    QLabel* m_outPathLabel = nullptr;
    QLineEdit* m_outDirEdit = nullptr;     // user-chosen output directory
    QPushButton* m_outDirBrowseBtn = nullptr;
    QComboBox* m_outputModeCombo = nullptr;
    QComboBox* m_outputBitsCombo = nullptr;
    QCheckBox* m_basicFilterCheck = nullptr;
    QLabel* m_filterProfileLabel = nullptr;
    QPushButton* m_processBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;   // stops an in-flight cut
    QProgressBar* m_progress = nullptr;
    QLabel* m_statusLabel = nullptr;

    // last computed plan + output path (filled in applyCut())
    FcPlan m_plan{};
    QString m_outPath;
    QFutureWatcher<FcChopResult>* m_watcher = nullptr;
    QFutureWatcher<FcProbe>* m_probeWatcher = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    bool m_syncing = false;
    bool m_probing = false; // true while fc_probe runs off-thread
    bool m_updateCheckInFlight = false;
    bool m_cancelRequested = false; // true while a cut cancel is pending
    bool m_outDirAutoFollow = true; // true: output dir follows the input file's dir until the user explicitly chooses one
};

#endif // FLACCHOP_MAINWINDOW_H
