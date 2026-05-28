#include "panoramasaver.h"

#include "panoramacache.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

PanoramaSaver::PanoramaSaver(QSharedPointer<PanoramaCache> cache, QObject *parent)
    : QObject(parent), m_cache(std::move(cache))
{
}

void PanoramaSaver::enqueueSave(QString outDir, int rgbJpegQuality)
{
    Job j;
    {
        QMutexLocker lk(&m_mtx);
        j.id = m_nextId++;
        j.outDir = std::move(outDir);
        j.rgbJpegQuality = rgbJpegQuality;
        m_jobs.enqueue(j);
    }
    schedule();
}

void PanoramaSaver::schedule()
{
    if (m_scheduled.testAndSetAcquire(0, 1)) {
        QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
    }
}

static bool writeImageFile(const QString &path, const QImage &img, const QByteArray &fmt, int quality, QString *err)
{
    QImageWriter w(path, fmt);
    if (quality >= 0) w.setQuality(quality);
    if (!w.write(img)) {
        if (err) *err = w.errorString();
        return false;
    }
    return true;
}

static bool readAllBytes(const QString &path, QByteArray *out, QString *err)
{
    if (out) out->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Open failed: %1").arg(path);
        return false;
    }
    const QByteArray b = f.readAll();
    if (b.isEmpty()) {
        if (err) *err = QStringLiteral("Empty file: %1").arg(path);
        return false;
    }
    if (out) *out = b;
    return true;
}

static QString safeExtFromPath(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().trimmed().toLower();
    if (ext.isEmpty()) return QStringLiteral("bin");
    return ext;
}

static QString padDec(int v, int width)
{
    QByteArray b = QByteArray::number(v);
    while (b.size() < width) b.prepend('0');
    return QString::fromLatin1(b);
}

void PanoramaSaver::process()
{
    m_scheduled.storeRelease(0);

    Job job;
    {
        QMutexLocker lk(&m_mtx);
        if (m_jobs.isEmpty()) return;
        job = m_jobs.dequeue();
    }

    if (!m_cache) {
        emit saveFinished(job.id, false, QStringLiteral("Cache not ready"), job.outDir);
        schedule();
        return;
    }

    const PanoramaCache::BlockState rgbFull = m_cache->state(PanoramaCache::FullRgb);
    const PanoramaCache::BlockState bwFull = m_cache->state(PanoramaCache::FullBw);
    if (!rgbFull.inited || !bwFull.inited || rgbFull.segments <= 0 || bwFull.segments <= 0) {
        emit saveFinished(job.id, false, QStringLiteral("Full panorama not ready"), job.outDir);
        schedule();
        return;
    }
    if (rgbFull.segments != bwFull.segments || rgbFull.panoW != bwFull.panoW || rgbFull.panoH != bwFull.panoH) {
        emit saveFinished(job.id, false, QStringLiteral("RGB/BW panorama mismatch"), job.outDir);
        schedule();
        return;
    }

    QDir().mkpath(job.outDir);

    // JPEG 单边最大 65535，我们留一点余量取 65500，保证 libjpeg 不卡限。
    const int JPEG_MAX_DIM = 65500;
    const int saveW = qMin(rgbFull.panoW, JPEG_MAX_DIM);
    const int saveH = rgbFull.panoH;
    const int segments = rgbFull.segments;
    const int sliceW = rgbFull.sliceW;

    // 一次性分配两张完整全景图（合并 tile）。
    QImage rgbCombined(saveW, saveH, QImage::Format_RGB32);
    if (rgbCombined.isNull()) {
        emit saveFinished(job.id, false, QStringLiteral("RGB allocate failed (out of memory)"), job.outDir);
        schedule();
        return;
    }
    rgbCombined.fill(Qt::black);

    // BW is stored as RGB32 (fake-gray RGB, see PanoramaCache). Compose it the
    // same way as RGB instead of as an 8-bit indexed image.
    QImage bwCombined(saveW, saveH, QImage::Format_RGB32);
    if (bwCombined.isNull()) {
        emit saveFinished(job.id, false, QStringLiteral("BW allocate failed (out of memory)"), job.outDir);
        schedule();
        return;
    }
    bwCombined.fill(Qt::black);

    const QByteArray env = qgetenv("PANO_SAVE_RAW");
    const bool saveRawBytes = (!env.isEmpty() && env != "0");

    QVector<quint64> rgbIdx;
    QVector<QString> rgbPaths;
    QVector<qint64> rgbRx;
    QVector<quint64> bwIdx;
    QVector<QString> bwPaths;
    QVector<qint64> bwRx;
    if (saveRawBytes) {
        m_cache->snapshotFullRgbTileSources(rgbIdx, rgbPaths, rgbRx);
        m_cache->snapshotFullBwTileSources(bwIdx, bwPaths, bwRx);
    }

    const QString rawDir = QDir(job.outDir).filePath(QStringLiteral("raw"));
    const QString rawRgbDir = QDir(rawDir).filePath(QStringLiteral("rgb"));
    const QString rawBwDir = QDir(rawDir).filePath(QStringLiteral("bw"));
    if (saveRawBytes) {
        QDir().mkpath(rawRgbDir);
        QDir().mkpath(rawBwDir);
    }
    QJsonArray rawRgb;
    QJsonArray rawBw;

    const qint64 composeStart = QDateTime::currentMSecsSinceEpoch();

    for (int tile = 0; tile < segments; ++tile) {
        const int startX = tile * sliceW;
        if (startX >= saveW) break;

        QImage rgbTile;
        if (!m_cache->copyFullRgbTile(tile, rgbTile)) {
            emit saveFinished(job.id, false, QStringLiteral("RGB tile copy failed"), job.outDir);
            schedule();
            return;
        }
        const int copyW = qMin(rgbTile.width(), saveW - startX);
        for (int y = 0; y < saveH; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb*>(rgbTile.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb*>(rgbCombined.scanLine(y)) + startX;
            memcpy(dst, src, (size_t)copyW * 4);
        }

        QImage bwTile;
        if (!m_cache->copyFullBwTile(tile, bwTile)) {
            emit saveFinished(job.id, false, QStringLiteral("BW tile copy failed"), job.outDir);
            schedule();
            return;
        }
        for (int y = 0; y < saveH; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb*>(bwTile.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb*>(bwCombined.scanLine(y)) + startX;
            memcpy(dst, src, (size_t)copyW * 4);
        }

        if (saveRawBytes && tile < rgbPaths.size() && !rgbPaths[tile].isEmpty()) {
            QByteArray bytes;
            QString err;
            if (readAllBytes(rgbPaths[tile], &bytes, &err)) {
                const QString ext = safeExtFromPath(rgbPaths[tile]);
                const QString rawName = QStringLiteral("rgb_") + padDec(tile, 4) + QStringLiteral(".") + ext;
                const QString rawPath = QDir(rawRgbDir).filePath(rawName);
                QFile rf(rawPath);
                if (rf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    rf.write(bytes);
                    rf.close();
                    QJsonObject ro;
                    ro.insert(QStringLiteral("tile"), tile);
                    ro.insert(QStringLiteral("file"), QStringLiteral("raw/rgb/") + rawName);
                    ro.insert(QStringLiteral("src"), rgbPaths[tile]);
                    ro.insert(QStringLiteral("fileIdx"), (tile < rgbIdx.size()) ? (qint64)rgbIdx[tile] : (qint64)0);
                    ro.insert(QStringLiteral("rxMs"), (tile < rgbRx.size()) ? (qint64)rgbRx[tile] : (qint64)0);
                    rawRgb.append(ro);
                }
            }
        }
        if (saveRawBytes && tile < bwPaths.size() && !bwPaths[tile].isEmpty()) {
            QByteArray bytes;
            QString err;
            if (readAllBytes(bwPaths[tile], &bytes, &err)) {
                const QString ext = safeExtFromPath(bwPaths[tile]);
                const QString rawName = QStringLiteral("bw_") + padDec(tile, 4) + QStringLiteral(".") + ext;
                const QString rawPath = QDir(rawBwDir).filePath(rawName);
                QFile rf(rawPath);
                if (rf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    rf.write(bytes);
                    rf.close();
                    QJsonObject ro;
                    ro.insert(QStringLiteral("tile"), tile);
                    ro.insert(QStringLiteral("file"), QStringLiteral("raw/bw/") + rawName);
                    ro.insert(QStringLiteral("src"), bwPaths[tile]);
                    ro.insert(QStringLiteral("fileIdx"), (tile < bwIdx.size()) ? (qint64)bwIdx[tile] : (qint64)0);
                    ro.insert(QStringLiteral("rxMs"), (tile < bwRx.size()) ? (qint64)bwRx[tile] : (qint64)0);
                    rawBw.append(ro);
                }
            }
        }
    }

    const qint64 composeMs = QDateTime::currentMSecsSinceEpoch() - composeStart;

    QString err;

    const qint64 rgbEncStart = QDateTime::currentMSecsSinceEpoch();
    const QString rgbPath = QDir(job.outDir).filePath(QStringLiteral("rgb.jpg"));
    if (!writeImageFile(rgbPath, rgbCombined, "jpg", job.rgbJpegQuality, &err)) {
        emit saveFinished(job.id, false, QStringLiteral("RGB JPG write failed: %1").arg(err), job.outDir);
        schedule();
        return;
    }
    const qint64 rgbBytes = QFileInfo(rgbPath).size();
    const qint64 rgbEncMs = QDateTime::currentMSecsSinceEpoch() - rgbEncStart;

    const qint64 bwEncStart = QDateTime::currentMSecsSinceEpoch();
    const QString bwPath = QDir(job.outDir).filePath(QStringLiteral("bw.jpg"));
    if (!writeImageFile(bwPath, bwCombined, "jpg", job.rgbJpegQuality, &err)) {
        emit saveFinished(job.id, false, QStringLiteral("BW JPG write failed: %1").arg(err), job.outDir);
        schedule();
        return;
    }
    const qint64 bwBytes = QFileInfo(bwPath).size();
    const qint64 bwEncMs = QDateTime::currentMSecsSinceEpoch() - bwEncStart;

    // 额外保存降采样预览图：原图 65500 宽超出绝大多数看图器上限，
    // 这里缩到 <=8192 宽，任何看图器都能秒开。全分辨率原图保留供算法使用。
    const int previewW = qMin(saveW, 8192);
    const int previewH = (saveW > 0) ? (int)((qint64)saveH * previewW / saveW) : saveH;
    qint64 rgbPreviewBytes = 0;
    qint64 bwPreviewBytes = 0;
    if (previewW > 0 && previewH > 0 && previewW < saveW) {
        QImage rgbPreview = rgbCombined.scaled(previewW, previewH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString rgbPreviewPath = QDir(job.outDir).filePath(QStringLiteral("rgb_preview.jpg"));
        if (writeImageFile(rgbPreviewPath, rgbPreview, "jpg", 88, &err))
            rgbPreviewBytes = QFileInfo(rgbPreviewPath).size();

        QImage bwPreview = bwCombined.scaled(previewW, previewH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString bwPreviewPath = QDir(job.outDir).filePath(QStringLiteral("bw_preview.jpg"));
        if (writeImageFile(bwPreviewPath, bwPreview, "jpg", 88, &err))
            bwPreviewBytes = QFileInfo(bwPreviewPath).size();
    }

    QJsonObject root;
    root.insert(QStringLiteral("saveId"), (qint64)job.id);
    root.insert(QStringLiteral("createdMs"), (qint64)QDateTime::currentMSecsSinceEpoch());
    root.insert(QStringLiteral("panoW"), saveW);
    root.insert(QStringLiteral("panoH"), saveH);
    root.insert(QStringLiteral("originalPanoW"), rgbFull.panoW);
    root.insert(QStringLiteral("croppedColumns"), rgbFull.panoW - saveW);
    root.insert(QStringLiteral("composeMs"), composeMs);
    root.insert(QStringLiteral("previewW"), previewW);
    root.insert(QStringLiteral("previewH"), previewH);

    QJsonObject rgbObj;
    rgbObj.insert(QStringLiteral("file"), QStringLiteral("rgb.jpg"));
    rgbObj.insert(QStringLiteral("preview"), QStringLiteral("rgb_preview.jpg"));
    rgbObj.insert(QStringLiteral("format"), QStringLiteral("jpg"));
    rgbObj.insert(QStringLiteral("quality"), job.rgbJpegQuality);
    rgbObj.insert(QStringLiteral("bytes"), rgbBytes);
    rgbObj.insert(QStringLiteral("previewBytes"), rgbPreviewBytes);
    rgbObj.insert(QStringLiteral("encodeMs"), rgbEncMs);
    root.insert(QStringLiteral("rgb"), rgbObj);

    QJsonObject bwObj;
    bwObj.insert(QStringLiteral("file"), QStringLiteral("bw.jpg"));
    bwObj.insert(QStringLiteral("preview"), QStringLiteral("bw_preview.jpg"));
    bwObj.insert(QStringLiteral("format"), QStringLiteral("jpg"));
    bwObj.insert(QStringLiteral("quality"), job.rgbJpegQuality);
    bwObj.insert(QStringLiteral("bytes"), bwBytes);
    bwObj.insert(QStringLiteral("previewBytes"), bwPreviewBytes);
    bwObj.insert(QStringLiteral("encodeMs"), bwEncMs);
    root.insert(QStringLiteral("bw"), bwObj);

    if (saveRawBytes) {
        root.insert(QStringLiteral("rawRgb"), rawRgb);
        root.insert(QStringLiteral("rawBw"), rawBw);
    }

    const QString manifestPath = QDir(job.outDir).filePath(QStringLiteral("manifest.json"));
    QFile mf(manifestPath);
    if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit saveFinished(job.id, false, QStringLiteral("Manifest write failed"), job.outDir);
        schedule();
        return;
    }
    mf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    mf.close();

    emit saveFinished(job.id, true,
        QString("OK panoW=%1 RGB=%2MB(%3ms) BW=%4MB(%5ms) compose=%6ms")
            .arg(saveW)
            .arg(rgbBytes / 1024 / 1024).arg(rgbEncMs)
            .arg(bwBytes / 1024 / 1024).arg(bwEncMs)
            .arg(composeMs),
        job.outDir);
    schedule();
}
