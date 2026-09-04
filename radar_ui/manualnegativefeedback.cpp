#include "manualnegativefeedback.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>

#include <cmath>

namespace {

QString normalizedClass(QString value)
{
    value = value.trimmed().toLower();
    value.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (value == QStringLiteral("airliner") || value == QStringLiteral("civilian_airliner")) {
        return QStringLiteral("civilian_airliners");
    }
    return value;
}

bool feedbackEnabled()
{
    if (!qEnvironmentVariableIsSet("DMX_MANUAL_NEGATIVE_FEEDBACK")) return true;
    const QString value = qEnvironmentVariable("DMX_MANUAL_NEGATIVE_FEEDBACK").trimmed().toLower();
    return value != QStringLiteral("0")
        && value != QStringLiteral("false")
        && value != QStringLiteral("off")
        && value != QStringLiteral("no");
}

int bitDistance(quint64 a, quint64 b)
{
    quint64 value = a ^ b;
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

quint64 appearanceHash(const TargetRecord &target, bool *ok)
{
    if (ok) *ok = false;
    if (!target.hasRoiBox || target.imagePath.isEmpty()) return 0;

    QImage source(target.imagePath);
    if (source.isNull()) return 0;
    const QRect imageRect(0, 0, source.width(), source.height());
    const QRect box(QPoint(target.roiBoxX1, target.roiBoxY1),
                    QPoint(target.roiBoxX2, target.roiBoxY2));
    const QRect normalizedBox = box.normalized().intersected(imageRect);
    if (normalizedBox.width() < 2 || normalizedBox.height() < 2) return 0;

    const QPoint center = normalizedBox.center();
    const int side = qMax(32, qMax(normalizedBox.width(), normalizedBox.height()) * 3);
    const QRect patchRect(center.x() - side / 2, center.y() - side / 2, side, side);
    QImage patch = source.copy(patchRect.intersected(imageRect))
        .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
    if (patch.width() != 9 || patch.height() != 8) return 0;

    quint64 hash = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(patch.constScanLine(y));
        for (int x = 0; x < 8; ++x, ++bit) {
            if (qGray(line[x]) > qGray(line[x + 1])) hash |= (quint64(1) << bit);
        }
    }
    if (ok) *ok = true;
    return hash;
}

double ageWeight(const QDateTime &markedAt, const QDateTime &now)
{
    if (!markedAt.isValid()) return 0.7;
    const double ageDays = qMax<qint64>(0, markedAt.secsTo(now)) / 86400.0;
    if (ageDays > 90.0) return 0.0;
    if (ageDays <= 14.0) return 1.0;
    return 1.0 - (ageDays - 14.0) / 76.0 * 0.45;
}

} // namespace

bool ManualNegativeFeedback::reload(const QString &rootPath, QString *error)
{
    if (error) error->clear();
    m_entries.clear();

    const QString manifestPath = QDir(rootPath).filePath(QStringLiteral("manifest.jsonl"));
    QFile manifest(manifestPath);
    if (!manifest.exists()) return true;
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法读取手动负样本反馈：%1").arg(manifestPath);
        return false;
    }

    while (!manifest.atEnd()) {
        const QByteArray line = manifest.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("sample_type")).toString()
            != QStringLiteral("manual_false_positive")) {
            continue;
        }

        Entry entry;
        entry.markedAt = QDateTime::fromString(
            object.value(QStringLiteral("marked_at")).toString(), Qt::ISODateWithMs);
        entry.className = normalizedClass(object.value(QStringLiteral("predicted_class")).toString());
        entry.stream = object.value(QStringLiteral("stream")).toString().trimmed().toUpper();
        entry.panoX = object.value(QStringLiteral("pano_x")).toInt(-1);
        entry.panoY = object.value(QStringLiteral("pano_y")).toInt(-1);
        if (entry.panoX < 0 || entry.panoY < 0) continue;

        bool hashOk = false;
        entry.appearanceHash = object.value(QStringLiteral("appearance_hash"))
            .toString().toULongLong(&hashOk, 16);
        entry.hasAppearanceHash = hashOk;
        m_entries.push_back(entry);
    }

    const int maxEntries = 1000;
    if (m_entries.size() > maxEntries) {
        m_entries = m_entries.mid(m_entries.size() - maxEntries);
    }
    return true;
}

ManualNegativeFeedbackResult ManualNegativeFeedback::apply(TargetRecord *target,
                                                           int panoramaWidth,
                                                           int panoramaHeight) const
{
    ManualNegativeFeedbackResult result;
    if (!target || m_entries.isEmpty() || !feedbackEnabled()) return result;

    result.originalConfidence = target->confidence;
    result.originalScore = target->score;
    if (!target->hasAppearanceHash) {
        target->appearanceHash = appearanceHash(*target, &target->hasAppearanceHash);
    }

    const int fullWidth = qMax(1, panoramaWidth);
    const int radiusX = qMax(256, fullWidth / 128);
    const int radiusY = qMax(128, qMax(1, panoramaHeight) / 16);
    const QString candidateClass = normalizedClass(target->className);
    const QDateTime now = QDateTime::currentDateTime();
    double strongest = 0.0;

    for (const Entry &entry : m_entries) {
        const double age = ageWeight(entry.markedAt, now);
        if (age <= 0.0) continue;

        int dx = qAbs(target->panoX - entry.panoX) % fullWidth;
        dx = qMin(dx, fullWidth - dx);
        const int dy = qAbs(target->panoY - entry.panoY);
        const double nx = (double)dx / (double)radiusX;
        const double ny = (double)dy / (double)radiusY;
        const double spatialDistance = std::sqrt(nx * nx + ny * ny);
        const double classWeight = candidateClass.isEmpty() || entry.className.isEmpty()
            || candidateClass == entry.className ? 1.0 : 0.9;

        double strength = 0.0;
        if (spatialDistance <= 1.0) {
            strength = 0.72 * (1.0 - spatialDistance * 0.45);
        }

        int hashDistance = -1;
        const double nearDistance = std::sqrt(nx * nx / 4.0 + ny * ny / 4.0);
        if (target->hasAppearanceHash && entry.hasAppearanceHash && nearDistance <= 1.0) {
            hashDistance = bitDistance(target->appearanceHash, entry.appearanceHash);
            if (hashDistance <= 12) {
                const double appearanceStrength = (0.82 - hashDistance * 0.03)
                    * (1.0 - nearDistance * 0.25);
                strength = qMax(strength, appearanceStrength);
            }
        }

        strength *= classWeight * age;
        if (strength < 0.12) continue;
        ++result.matchCount;
        if (strength > strongest) {
            strongest = strength;
            result.bestDx = dx;
            result.bestDy = dy;
            result.bestHashDistance = hashDistance;
        }
    }

    if (result.matchCount <= 0) return result;

    const double repeatBoost = qMin(0.18,
        std::log((double)result.matchCount + 1.0) / std::log(2.0) * 0.045);
    result.weight = qBound(0.18, 1.0 - strongest - repeatBoost, 1.0);
    target->confidence *= result.weight;
    target->classConfidence *= result.weight;
    target->score *= result.weight;
    target->imageConfidence *= result.weight;

    const bool strongTarget = result.originalConfidence >= 0.72;
    result.suppressed = !strongTarget
        && (target->confidence < 0.16 || target->score < 42.0);
    return result;
}

int ManualNegativeFeedback::sampleCount() const
{
    return m_entries.size();
}
