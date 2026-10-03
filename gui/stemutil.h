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

#endif // FLACCHOP_STEMUTIL_H
