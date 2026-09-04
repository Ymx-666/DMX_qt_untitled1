#include "manualnegativestore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString safeFileToken(const QString &value, const QString &fallback)
{
    QString result;
    result.reserve(value.size());
    for (const QChar ch : value.trimmed()) {
        const ushort code = ch.unicode();
        const bool asciiAlphaNumeric = (code >= '0' && code <= '9')
            || (code >= 'A' && code <= 'Z')
            || (code >= 'a' && code <= 'z');
        result.append(asciiAlphaNumeric || ch == QLatin1Char('-') || ch == QLatin1Char('_')
            ? ch
            : QLatin1Char('_'));
    }
    while (result.contains(QStringLiteral("__"))) result.replace(QStringLiteral("__"), QStringLiteral("_"));
    result = result.trimmed();
    while (result.startsWith(QLatin1Char('_'))) result.remove(0, 1);
    while (result.endsWith(QLatin1Char('_'))) result.chop(1);
    return result.isEmpty() ? fallback : result;
}

QString uniqueBaseName(const QDir &imageDir,
                       const QDir &labelDir,
                       const TargetRecord &target)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString prefix = QStringLiteral("manual_neg_%1_%2_%3_%4")
        .arg(timestamp,
             safeFileToken(target.id, QStringLiteral("target")),
             safeFileToken(target.className, QStringLiteral("unknown")),
             safeFileToken(target.stream, QStringLiteral("stream")));
    QString candidate = prefix;
    int suffix = 1;
    while (QFileInfo::exists(imageDir.filePath(candidate + QStringLiteral(".jpg")))
           || QFileInfo::exists(labelDir.filePath(candidate + QStringLiteral(".txt")))) {
        candidate = prefix + QStringLiteral("_%1").arg(suffix++);
    }
    return candidate;
}

QString isoDate(const QDateTime &value)
{
    return value.isValid() ? value.toString(Qt::ISODateWithMs) : QString();
}

bool writeReadme(const QDir &root)
{
    const QString path = root.filePath(QStringLiteral("README.md"));
    if (QFileInfo::exists(path)) return true;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray content(
        "# DMX Manual Negative Samples\n\n"
        "- `images/`: clean target ROI images marked as false positives.\n"
        "- `labels/`: matching empty YOLO label files.\n"
        "- `manifest.jsonl`: source prediction and traceability metadata.\n\n"
        "Use each image together with its empty label when building a YOLO training set.\n");
    if (file.write(content) != content.size()) return false;
    return file.commit();
}

} // namespace

QString ManualNegativeStore::defaultRootPath()
{
    const QString configured = qEnvironmentVariable("DMX_MANUAL_NEGATIVE_ROOT").trimmed();
    if (!configured.isEmpty()) return QDir::cleanPath(configured);
    return QStringLiteral("/mnt/dmx4t/data/manual_negative_samples");
}

ManualNegativeSaveResult ManualNegativeStore::save(const TargetRecord &target,
                                                    const QString &rootPath,
                                                    const QString &modelPath)
{
    ManualNegativeSaveResult result;
    result.rootPath = QDir::cleanPath(rootPath.trimmed().isEmpty() ? defaultRootPath() : rootPath);

    const QFileInfo sourceInfo(target.imagePath);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
        result.error = QStringLiteral("目标原图不存在或不可读：%1").arg(target.imagePath);
        return result;
    }

    const QImage sourceImage(target.imagePath);
    if (sourceImage.isNull()) {
        result.error = QStringLiteral("目标原图无法解码：%1").arg(target.imagePath);
        return result;
    }

    QDir root(result.rootPath);
    if (!root.mkpath(QStringLiteral("images")) || !root.mkpath(QStringLiteral("labels"))) {
        result.error = QStringLiteral("无法创建负样本目录：%1").arg(result.rootPath);
        return result;
    }

    const QDir imageDir(root.filePath(QStringLiteral("images")));
    const QDir labelDir(root.filePath(QStringLiteral("labels")));
    const QString baseName = uniqueBaseName(imageDir, labelDir, target);
    result.imagePath = imageDir.filePath(baseName + QStringLiteral(".jpg"));
    result.labelPath = labelDir.filePath(baseName + QStringLiteral(".txt"));
    result.manifestPath = root.filePath(QStringLiteral("manifest.jsonl"));

    if (!sourceImage.save(result.imagePath, "JPG", 95)) {
        result.error = QStringLiteral("负样本图像写入失败：%1").arg(result.imagePath);
        return result;
    }

    QSaveFile labelFile(result.labelPath);
    if (!labelFile.open(QIODevice::WriteOnly | QIODevice::Text) || !labelFile.commit()) {
        QFile::remove(result.imagePath);
        result.error = QStringLiteral("空 YOLO 标签写入失败：%1").arg(result.labelPath);
        return result;
    }

    QJsonObject metadata;
    metadata.insert(QStringLiteral("schema_version"), 1);
    metadata.insert(QStringLiteral("sample_type"), QStringLiteral("manual_false_positive"));
    metadata.insert(QStringLiteral("marked_at"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    metadata.insert(QStringLiteral("target_id"), target.id);
    metadata.insert(QStringLiteral("predicted_class"), target.className);
    metadata.insert(QStringLiteral("stream"), target.stream);
    metadata.insert(QStringLiteral("last_stream"), target.lastStream);
    metadata.insert(QStringLiteral("state"), target.state);
    metadata.insert(QStringLiteral("confidence"), target.confidence);
    metadata.insert(QStringLiteral("class_confidence"), target.classConfidence);
    metadata.insert(QStringLiteral("score"), target.score);
    metadata.insert(QStringLiteral("hits"), target.hits);
    metadata.insert(QStringLiteral("azimuth_deg"), target.azimuthDeg);
    metadata.insert(QStringLiteral("pano_x"), target.panoX);
    metadata.insert(QStringLiteral("pano_y"), target.panoY);
    metadata.insert(QStringLiteral("frame_x"), target.frameX);
    metadata.insert(QStringLiteral("frame_y"), target.frameY);
    metadata.insert(QStringLiteral("frame_key"), target.frameKey);
    metadata.insert(QStringLiteral("target_time"), isoDate(target.time));
    metadata.insert(QStringLiteral("first_time"), isoDate(target.firstTime));
    metadata.insert(QStringLiteral("last_time"), isoDate(target.lastTime));
    metadata.insert(QStringLiteral("source_image"), sourceInfo.absoluteFilePath());
    metadata.insert(QStringLiteral("saved_image"), result.imagePath);
    metadata.insert(QStringLiteral("saved_label"), result.labelPath);
    metadata.insert(QStringLiteral("image_width"), sourceImage.width());
    metadata.insert(QStringLiteral("image_height"), sourceImage.height());
    metadata.insert(QStringLiteral("model_path"), modelPath);
    if (target.hasRoiBox) {
        metadata.insert(QStringLiteral("predicted_box"), QJsonArray()
            << target.roiBoxX1 << target.roiBoxY1 << target.roiBoxX2 << target.roiBoxY2);
    }
    if (target.hasAppearanceHash) {
        metadata.insert(QStringLiteral("appearance_hash"),
                        QString::number(target.appearanceHash, 16).rightJustified(16, QLatin1Char('0')));
    }

    QByteArray line = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    line.append('\n');
    QFile manifest(result.manifestPath);
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)
        || manifest.write(line) != line.size()) {
        manifest.close();
        QFile::remove(result.imagePath);
        QFile::remove(result.labelPath);
        result.error = QStringLiteral("负样本元数据写入失败：%1").arg(result.manifestPath);
        return result;
    }
    manifest.close();

    writeReadme(root);
    result.success = true;
    return result;
}
