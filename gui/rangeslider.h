#ifndef FLACCHOP_RANGESLIDER_H
#define FLACCHOP_RANGESLIDER_H

#include <QWidget>

// Qt has no built-in two-handle range slider, so this is a self-contained one.
// Values are plain ints in whatever unit the caller chooses (here: deciseconds).
// Invariant: out >= in + minSpan (default 1), so the selection is never empty.
class QRangeSlider : public QWidget {
    Q_OBJECT
public:
    explicit QRangeSlider(QWidget* parent = nullptr);

    void setRange(int min, int max);
    int minimum() const { return m_min; }
    int maximum() const { return m_max; }

    void setInValue(int v);
    void setOutValue(int v);
    int inValue() const { return m_in; }
    int outValue() const { return m_out; }

    void setMinSpan(int span);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void inValueChanged(int v);
    void outValueChanged(int v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    enum class Hit { None, In, Out, Between };

    int clampIn(int v) const;
    int clampOut(int v) const;
    int valueFromX(int x) const;
    int xFromValue(int v) const;
    int handleHalfWidth() const { return 6; }
    Hit hitTest(int x) const;

    int m_min = 0;
    int m_max = 100;
    int m_in = 0;
    int m_out = 100;
    int m_minSpan = 1;
    int m_spanAtPress = 0;
    Hit m_drag = Hit::None;
    int m_dragXOffset = 0;
};

#endif // FLACCHOP_RANGESLIDER_H
