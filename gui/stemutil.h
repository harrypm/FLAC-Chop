#ifndef FLACCHOP_STEMUTIL_H
#define FLACCHOP_STEMUTIL_H

// Shared output-stem naming, used by both the Chop tab and the Batch Task
// tab so single-file cuts and batch outputs are named identically.

#include <QString>
#include <QFileInfo>
#include <QRegularExpression>

// Rename the output stem to reflect the new altered metadata when the input
// name matches the MISRC capture naming convention, which (per MISRC-GUI
// gui_settings.c) is:  <base>_<rfTag>_<B>-bit_<N>msps[.flac]
// i.e. bits first, then rate — e.g.  ..._8-bit_20msps  ->  ..._6-bit_16msps.
// "Keep source rate/bits" (0) keeps the original token. A non-matching name
// returns "" (the caller then uses the stock <stem>-cut suffix).
inline QString renamedOutputStem(const QString& inPath, quint64 outHeaderHz, uint outBits)
{
    if (inPath.isEmpty())
        return QString();
    const QString inStem = QFileInfo(inPath).completeBaseName();
    // Match an optional prefix, then <B>-bit_<N>msps, then an optional suffix.
    static const QRegularExpression re(QStringLiteral("^(.*?)([0-9]+)-bit_([0-9]+)msps(.*)$"));
    const auto m = re.match(inStem);
    if (!m.hasMatch())
        return QString();
    const QString prefix = m.captured(1);
    const QString srcBitsTok = m.captured(2);
    const QString srcMspsTok = m.captured(3);
    const QString suffix = m.captured(4);

    // New bits token: the selected output bit-depth, or keep the source token
    // when "keep source bit-depth".
    QString bitsTok = srcBitsTok;
    if (outBits > 0)
        bitsTok = QString::number(outBits);

    // New rate token: the selected output mode (header kHz / 1000 = MSPS), or
    // keep the source token when "keep source rate".
    QString mspsTok = srcMspsTok;
    if (outHeaderHz > 0)
        mspsTok = QString::number(outHeaderHz / 1000);

    return prefix + bitsTok + QStringLiteral("-bit_") + mspsTok + QStringLiteral("msps") + suffix;
}

// Format an elapsed time in milliseconds for the finished-job status lines
// (shared by the Chop / Batch Task / Sync Edit tabs): "0.8 s", "42 s",
// "2m 31s", "1h 04m 12s".
inline QString formatElapsedMSecs(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const double secs = ms / 1000.0;
    if (secs < 10.0)
        return QStringLiteral("%1 s").arg(secs, 0, 'f', 1);
    const qint64 total = qint64(secs + 0.5);
    if (total < 60)
        return QStringLiteral("%1 s").arg(total);
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;
    if (h > 0)
        return QStringLiteral("%1h %2m %3s")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1m %2s")
        .arg(m)
        .arg(s, 2, 10, QLatin1Char('0'));
}

#endif // FLACCHOP_STEMUTIL_H
