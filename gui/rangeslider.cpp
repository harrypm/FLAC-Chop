#include "rangeslider.h"

#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <cmath>

QRangeSlider::QRangeSlider(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void QRangeSlider::setRange(int min, int max)
{
    if (max < min) max = min;
    m_min = min;
    m_max = max;
    if (m_in < m_min) m_in = m_min;
    if (m_out > m_max) m_out = m_max;
    if (m_out < m_in + m_minSpan) m_out = qMin(m_max, m_in + m_minSpan);
    if (m_in > m_out - m_minSpan) m_in = qMax(m_min, m_out - m_minSpan);
    update();
}

void QRangeSlider::setMinSpan(int span) { m_minSpan = qMax(1, span); }

int QRangeSlider::clampIn(int v) const
{
    if (m_max <= m_min) return m_min;
    return qBound(m_min, v, m_out - m_minSpan);
}

int QRangeSlider::clampOut(int v) const
{
    if (m_max <= m_min) return m_max;
    return qBound(m_in + m_minSpan, v, m_max);
}

void QRangeSlider::setInValue(int v)
{
    v = clampIn(v);
    if (v != m_in) {
        m_in = v;
        update();
        emit inValueChanged(m_in);
    }
}

void QRangeSlider::setOutValue(int v)
{
    v = clampOut(v);
    if (v != m_out) {
        m_out = v;
        update();
        emit outValueChanged(m_out);
    }
}

int QRangeSlider::xFromValue(int v) const
{
    const int pad = handleHalfWidth() + 2;
    const int usable = width() - 2 * pad;
    if (m_max <= m_min || usable <= 0) return pad;
    return pad + int(std::lround(double(v - m_min) / double(m_max - m_min) * usable));
}

int QRangeSlider::valueFromX(int x) const
{
    const int pad = handleHalfWidth() + 2;
    const int usable = width() - 2 * pad;
    if (m_max <= m_min || usable <= 0) return m_min;
    int v = m_min + int(std::lround(double(x - pad) / double(usable) * (m_max - m_min)));
    return qBound(m_min, v, m_max);
}

QRangeSlider::Hit QRangeSlider::hitTest(int x) const
{
    const int xin = xFromValue(m_in);
    const int xout = xFromValue(m_out);
    const int hw = handleHalfWidth();
    if (std::abs(x - xin) <= hw) return Hit::In;
    if (std::abs(x - xout) <= hw) return Hit::Out;
    if (x > xin + hw && x < xout - hw) return Hit::Between;
    return Hit::None;
}

void QRangeSlider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r = rect();
    const int pad = handleHalfWidth() + 2;
    const int trackY = r.height() / 2;
    const int trackH = 6;

    // base track
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Window).darker(140));
    p.drawRoundedRect(QRect(pad, trackY - trackH / 2, r.width() - 2 * pad, trackH), 3, 3);

    const int xin = xFromValue(m_in);
    const int xout = xFromValue(m_out);
    const QColor accent = palette().color(QPalette::Highlight);

    // selected range (neutral dim bar so the red/green handles read clearly)
    p.setBrush(accent.darker(150));
    p.drawRoundedRect(QRect(xin, trackY - trackH / 2, qMax(1, xout - xin), trackH), 3, 3);

    // handles — IN (start) green, OUT (end) red so the cut posts are
    // distinguishable at a glance (per user request).
    const QColor inColor(60, 200, 80);   // green
    const QColor outColor(220, 60, 60);  // red
    const auto drawHandle = [&](int x, const QColor& base, bool active) {
        QRect hr(x - handleHalfWidth(), trackY - 10, handleHalfWidth() * 2, 20);
        p.setBrush(active ? base.lighter(150) : base);
        p.setPen(QPen(palette().color(QPalette::WindowText), 1));
        p.drawRoundedRect(hr, 4, 4);
    };
    drawHandle(xin, inColor, m_drag == Hit::In);
    drawHandle(xout, outColor, m_drag == Hit::Out);
}

void QRangeSlider::mousePressEvent(QMouseEvent* e)
{
    if (m_max <= m_min) return;
    const int x = e->pos().x();
    Hit h = hitTest(x);
    if (h == Hit::None) {
        // empty area: jump the nearer handle to the click
        const int v = valueFromX(x);
        h = (std::abs(v - m_in) <= std::abs(v - m_out)) ? Hit::In : Hit::Out;
    }
    m_drag = h;
    m_spanAtPress = m_out - m_in;
    if (h == Hit::In) {
        setInValue(valueFromX(x));
        m_dragXOffset = x - xFromValue(m_in);
    } else if (h == Hit::Out) {
        setOutValue(valueFromX(x));
        m_dragXOffset = x - xFromValue(m_out);
    } else {
        // Between: drag the whole selection, preserving span
        m_dragXOffset = x - xFromValue(m_in);
    }
    setFocus();
    update();
}

void QRangeSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag == Hit::None) return;
    const int x = e->pos().x();
    if (m_drag == Hit::In) {
        setInValue(valueFromX(x - m_dragXOffset));
    } else if (m_drag == Hit::Out) {
        setOutValue(valueFromX(x - m_dragXOffset));
    } else {
        const int span = m_spanAtPress;
        int newIn = valueFromX(x - m_dragXOffset);
        int newOut = newIn + span;
        if (newOut > m_max) { newOut = m_max; newIn = newOut - span; }
        if (newIn < m_min) { newIn = m_min; newOut = newIn + span; if (newOut > m_max) newOut = m_max; }
        setInValue(newIn);
        setOutValue(newOut);
    }
}

void QRangeSlider::mouseReleaseEvent(QMouseEvent*)
{
    m_drag = Hit::None;
    update();
}

void QRangeSlider::keyPressEvent(QKeyEvent* e)
{
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    const Hit active = (m_drag != Hit::None) ? m_drag : Hit::In;
    switch (e->key()) {
        case Qt::Key_Left:
            if (active == Hit::Out) setOutValue(m_out - step);
            else setInValue(m_in - step);
            break;
        case Qt::Key_Right:
            if (active == Hit::Out) setOutValue(m_out + step);
            else setInValue(m_in + step);
            break;
        default:
            QWidget::keyPressEvent(e);
    }
}

QSize QRangeSlider::sizeHint() const { return QSize(420, 30); }
QSize QRangeSlider::minimumSizeHint() const { return QSize(120, 24); }
