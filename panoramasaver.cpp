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
    const QString rgbDir = QDir(job.outDir).filePath(QStringLiteral("rgb"));
    const QString bwDir = QDir(job.outDir).filePath(QStringLiteral("bw"));
    QDir().mkpath(rgbDir);
    QDir().mkpath(bwDir);

    QJsonObject root;
    root.insert(QStringLiteral("saveId"), (qint64)job.id);
    root.insert(QStringLiteral("createdMs"), (qint64)QDateTime::currentMSecsSinceEpoch());
    root.insert(QStringLiteral("panoW"), rgbFull.panoW);
    root.insert(QStringLiteral("panoH"), rgbFull.panoH);
    root.insert(QStringLiteral("segments"), rgbFull.segments);
    root.insert(QStringLiteral("sliceW"), rgbFull.sliceW);

    QJsonObject rgbObj;
    rgbObj.insert(QStringLiteral("format"), QStringLiteral("jpg"));
    rgbObj.insert(QStringLiteral("quality"), job.rgbJpegQuality);
    QJsonArray rgbTiles;

    QJsonObject bwObj;
    bwObj.insert(QStringLiteral("format"), QStringLiteral("png"));
    QJsonArray bwTiles;

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

    QString err;
    for (int tile = 0; tile < rgbFull.segments; ++tile) {
        QImage rgbTile;
        if (!m_cache->copyFullRgbTile(tile, rgbTile)) {
            emit saveFinished(job.id, false, QStringLiteral("RGB tile copy failed"), job.outDir);
            schedule();
            return;
        }
        const QString rgbName = QStringLiteral("rgb_") + padDec(tile, 4) + QStringLiteral(".jpg");
        const QString rgbPath = QDir(rgbDir).filePath(rgbName);
        err.clear();
        if (!writeImageFile(rgbPath, rgbTile, "jpg", job.rgbJpegQuality, &err)) {
            emit saveFinished(job.id, false, QStringLiteral("RGB write failed: %1").arg(err), job.outDir);
            schedule();
            return;
        }
        const QFileInfo rfi(rgbPath);
        QJsonObject r;
        r.insert(QStringLiteral("tile"), tile);
        r.insert(QStringLiteral("file"), rgbName);
        r.insert(QStringLiteral("bytes"), (qint64)rfi.size());
        rgbTiles.append(r);

        if (saveRawBytes && tile < rgbPaths.size() && !rgbPaths[tile].isEmpty()) {
            QByteArray bytes;
            err.clear();
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
                    ro.insert(QStringLiteral("file"), rawName);
                    ro.insert(QStringLiteral("src"), rgbPaths[tile]);
                    ro.insert(QStringLiteral("fileIdx"), (tile < rgbIdx.size()) ? (qint64)rgbIdx[tile] : (qint64)0);
                    ro.insert(QStringLiteral("rxMs"), (tile < rgbRx.size()) ? (qint64)rgbRx[tile] : (qint64)0);
                    rawRgb.append(ro);
                }
            }
        }

        QImage bwTile;
        if (!m_cache->copyFullBwTile(tile, bwTile)) {
            emit saveFinished(job.id, false, QStringLiteral("BW tile copy failed"), job.outDir);
            schedule();
            return;
        }
        const QString bwName = QStringLiteral("bw_") + padDec(tile, 4) + QStringLiteral(".png");
        const QString bwPath = QDir(bwDir).filePath(bwName);
        err.clear();
        if (!writeImageFile(bwPath, bwTile, "png", -1, &err)) {
            emit saveFinished(job.id, false, QStringLiteral("BW write failed: %1").arg(err), job.outDir);
            schedule();
            return;
        }
        const QFileInfo bfi(bwPath);
        QJsonObject b;
        b.insert(QStringLiteral("tile"), tile);
        b.insert(QStringLiteral("file"), bwName);
        b.insert(QStringLiteral("bytes"), (qint64)bfi.size());
        bwTiles.append(b);

        if (saveRawBytes && tile < bwPaths.size() && !bwPaths[tile].isEmpty()) {
            QByteArray bytes;
            err.clear();
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
                    ro.insert(QStringLiteral("file"), rawName);
                    ro.insert(QStringLiteral("src"), bwPaths[tile]);
                    ro.insert(QStringLiteral("fileIdx"), (tile < bwIdx.size()) ? (qint64)bwIdx[tile] : (qint64)0);
                    ro.insert(QStringLiteral("rxMs"), (tile < bwRx.size()) ? (qint64)bwRx[tile] : (qint64)0);
                    rawBw.append(ro);
                }
            }
        }
    }

    rgbObj.insert(QStringLiteral("tiles"), rgbTiles);
    bwObj.insert(QStringLiteral("tiles"), bwTiles);
    root.insert(QStringLiteral("rgb"), rgbObj);
    root.insert(QStringLiteral("bw"), bwObj);
    root.insert(QStringLiteral("rawRgb"), rawRgb);
    root.insert(QStringLiteral("rawBw"), rawBw);

    const QString manifestPath = QDir(job.outDir).filePath(QStringLiteral("manifest.json"));
    QFile mf(manifestPath);
    if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit saveFinished(job.id, false, QStringLiteral("Manifest write failed"), job.outDir);
        schedule();
        return;
    }
    mf.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    mf.close();

    emit saveFinished(job.id, true, QStringLiteral("OK"), job.outDir);
    schedule();
}
