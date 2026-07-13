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
    QLabel* m_pathLabel = nullptr;
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
    QPushButton* m_processBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_statusLabel = nullptr;

    // last computed plan + output path (filled in applyCut())
    FcPlan m_plan{};
    QString m_outPath;
    QFutureWatcher<FcChopResult>* m_watcher = nullptr;
    QFutureWatcher<FcProbe>* m_probeWatcher = nullptr;
    bool m_syncing = false;
    bool m_probing = false; // true while fc_probe runs off-thread
};

#endif // FLACCHOP_MAINWINDOW_H
