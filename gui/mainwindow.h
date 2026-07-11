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
    void recompute();
    void process();
    void onChopFinished();
    void onSliderInChanged(int v);
    void onSliderOutChanged(int v);

private:
    // HH:MM:SS parsing helpers (accept "SS", "MM:SS", "HH:MM:SS").
    static bool parseHms(const QString& s, double& outSec);
    static QString secsToHms(double s);
    void loadFile(const QString& fn);
    void setProbeInfo();
    void setControlsEnabled(bool enabled);
    void syncSliderFromEdits();

    QString m_inPath;
    FcProbe m_probe{};
    bool m_probeOk = false;
    int m_sliderMaxDs = 0;   // slider range in deciseconds (0.1 s)

    // owned widgets
    QLabel* m_pathLabel = nullptr;
    QPushButton* m_browseBtn = nullptr;
    QLineEdit* m_inEdit = nullptr;
    QLineEdit* m_lenEdit = nullptr;
    QLabel* m_outLabel = nullptr;
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

    // last computed plan + output path (filled in recompute())
    FcPlan m_plan{};
    QString m_outPath;
    QFutureWatcher<FcChopResult>* m_watcher = nullptr;
    bool m_syncing = false;
};

#endif // FLACCHOP_MAINWINDOW_H
