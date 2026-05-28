#include "panoramacache.h"

#include <QtMath>
#include <QFile>
#include <QDataStream>
#include <algorithm>

static QVector<QRgb> grayColorTableLocal()
{
    QVector<QRgb> table(256);
    for (int i = 0; i < 256; ++i) table[i] = qRgb(i, i, i);
    return table;
}

PanoramaCache::PanoramaCache()
{
    m_rgb.thumbH = 240;
    m_bw.thumbH = 240;
}

PanoramaCache::~PanoramaCache()
{
    m_rgb.segLocks.clear();
    m_bw.segLocks.clear();
}

void PanoramaCache::configureFullSize(int w, int h)
{
    QWriteLocker lr(&m_rgb.lock);
    QWriteLocker lb(&m_bw.lock);
    m_rgb.fullW = w;
    m_rgb.fullH = h;
    m_bw.fullW = w;
    m_bw.fullH = h;
    m_rgb.fullReady = false;
    m_bw.fullReady = false;
    m_rgb.inited = false;
    m_bw.inited = false;
}

void PanoramaCache::configureThumbSize(int w, int h)
{
    QWriteLocker lr(&m_rgb.lock);
    QWriteLocker lb(&m_bw.lock);
    m_rgb.thumbW = w;
    m_rgb.thumbH = h;
    m_bw.thumbW = w;
    m_bw.thumbH = h;
    m_rgb.fullReady = false;
    m_bw.fullReady = false;
    m_rgb.inited = false;
    m_bw.inited = false;
}

void PanoramaCache::resetRgb()
{
    QWriteLocker l(&m_rgb.lock);
    m_rgb.segLocks.clear();
    m_rgb.full = QImage();
    m_rgb.thumb = QImage();
    m_rgb.segments = 0;
    m_rgb.sliceWFull = 0;
    m_rgb.sliceWThumb = 0;
    m_rgb.writeIndex = 0;
    m_rgb.validFrames = 0;
    m_rgb.hasWrapped = false;
    m_rgb.totalFrames = 0;
    m_rgb.droppedFrames = 0;
    m_rgb.fullReady = false;
    m_rgb.inited = false;
}

void PanoramaCache::resetBw()
{
    QWriteLocker l(&m_bw.lock);
    m_bw.segLocks.clear();
    m_bw.full = QImage();
    m_bw.thumb = QImage();
    m_bw.segments = 0;
    m_bw.sliceWFull = 0;
    m_bw.sliceWThumb = 0;
    m_bw.writeIndex = 0;
    m_bw.validFrames = 0;
    m_bw.hasWrapped = false;
    m_bw.totalFrames = 0;
    m_bw.droppedFrames = 0;
    m_bw.fullReady = false;
    m_bw.inited = false;
}

void PanoramaCache::resetAll()
{
    QWriteLocker lr(&m_rgb.lock);
    QWriteLocker lb(&m_bw.lock);
    m_rgb.segLocks.clear();
    m_bw.segLocks.clear();
    m_rgb.full = QImage();
    m_rgb.thumb = QImage();
    m_rgb.segments = 0;
    m_rgb.sliceWFull = 0;
    m_rgb.sliceWThumb = 0;
    m_rgb.writeIndex = 0;
    m_rgb.validFrames = 0;
    m_rgb.hasWrapped = false;
    m_rgb.totalFrames = 0;
    m_rgb.droppedFrames = 0;
    m_rgb.fullReady = false;
    m_rgb.inited = false;

    m_bw.full = QImage();
    m_bw.thumb = QImage();
    m_bw.segments = 0;
    m_bw.sliceWFull = 0;
    m_bw.sliceWThumb = 0;
    m_bw.writeIndex = 0;
    m_bw.validFrames = 0;
    m_bw.hasWrapped = false;
    m_bw.totalFrames = 0;
    m_bw.droppedFrames = 0;
    m_bw.fullReady = false;
    m_bw.inited = false;
}

void PanoramaCache::pushRgbFrame(const QImage &frameRgb32)
{
    pushFrameInternal(m_rgb, frameRgb32, true, 0, QString(), 0, -1.0);
}

void PanoramaCache::pushBwFrame(const QImage &frameBwIndexed8)
{
    pushFrameInternal(m_bw, frameBwIndexed8, false, 0, QString(), 0, -1.0);
}

void PanoramaCache::pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex)
{
    pushFrameInternal(m_rgb, frameRgb32, true, fileIndex, QString(), 0, -1.0);
}

void PanoramaCache::pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex)
{
    pushFrameInternal(m_bw, frameBwIndexed8, false, fileIndex, QString(), 0, -1.0);
}

void PanoramaCache::pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex, const QString &sourcePath, qint64 rxMs)
{
    pushFrameInternal(m_rgb, frameRgb32, true, fileIndex, sourcePath, rxMs, -1.0);
}

void PanoramaCache::pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex, const QString &sourcePath, qint64 rxMs)
{
    pushFrameInternal(m_bw, frameBwIndexed8, false, fileIndex, sourcePath, rxMs, -1.0);
}

void PanoramaCache::pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg)
{
    pushFrameInternal(m_rgb, frameRgb32, true, fileIndex, sourcePath, rxMs, angleDeg);
}

void PanoramaCache::pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg)
{
    pushFrameInternal(m_bw, frameBwIndexed8, false, fileIndex, sourcePath, rxMs, angleDeg);
}

QImage PanoramaCache::snapshotThumbRgb() const
{
    return snapshotInternalThumb(m_rgb);
}

QImage PanoramaCache::snapshotThumbBw() const
{
    return snapshotInternalThumb(m_bw);
}

QImage PanoramaCache::extractFullRgbSliceByAngle(double angleDeg) const
{
    return extractSliceInternal(m_rgb, angleDeg, false);
}

QImage PanoramaCache::extractFullBwSliceByAngle(double angleDeg, bool allow180Fallback) const
{
    return extractSliceInternal(m_bw, angleDeg, allow180Fallback);
}

QImage PanoramaCache::extractThumbRgbSliceByAngle(double angleDeg) const
{
    return extractThumbSliceInternal(m_rgb, angleDeg, false);
}

QImage PanoramaCache::extractThumbBwSliceByAngle(double angleDeg, bool allow180Fallback) const
{
    return extractThumbSliceInternal(m_bw, angleDeg, allow180Fallback);
}

QImage PanoramaCache::extractFullRgbWindow(double centerAngleDeg, int windowW) const
{
    return extractWindowInternal(m_rgb, true, centerAngleDeg, windowW);
}

QImage PanoramaCache::extractFullBwWindow(double centerAngleDeg, int windowW) const
{
    return extractWindowInternal(m_bw, true, centerAngleDeg, windowW);
}

QImage PanoramaCache::extractThumbRgbWindow(double centerAngleDeg, int windowW) const
{
    return extractWindowInternal(m_rgb, false, centerAngleDeg, windowW);
}

QImage PanoramaCache::extractThumbBwWindow(double centerAngleDeg, int windowW) const
{
    return extractWindowInternal(m_bw, false, centerAngleDeg, windowW);
}

bool PanoramaCache::copyFullRgbTile(int tileIndex, QImage &out) const
{
    return copyFullTileInternal(m_rgb, tileIndex, out);
}

bool PanoramaCache::copyFullBwTile(int tileIndex, QImage &out) const
{
    return copyFullTileInternal(m_bw, tileIndex, out);
}

bool PanoramaCache::snapshotFullRgbTileSources(QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const
{
    return snapshotFullTileSourcesInternal(m_rgb, fileIdx, paths, rxMs);
}

bool PanoramaCache::snapshotFullBwTileSources(QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const
{
    return snapshotFullTileSourcesInternal(m_bw, fileIdx, paths, rxMs);
}

PanoramaCache::ThumbInfo PanoramaCache::thumbInfoRgb() const
{
    return thumbInfoInternal(m_rgb);
}

PanoramaCache::ThumbInfo PanoramaCache::thumbInfoBw() const
{
    return thumbInfoInternal(m_bw);
}

QVector<int> PanoramaCache::takeDirtyThumbRgbTiles()
{
    return takeDirtyThumbTilesInternal(m_rgb);
}

QVector<int> PanoramaCache::takeDirtyThumbBwTiles()
{
    return takeDirtyThumbTilesInternal(m_bw);
}

bool PanoramaCache::blitThumbRgbTileTo(int tileIndex, QImage &dst) const
{
    return blitThumbTileInternal(m_rgb, tileIndex, dst);
}

bool PanoramaCache::blitThumbBwTileTo(int tileIndex, QImage &dst) const
{
    return blitThumbTileInternal(m_bw, tileIndex, dst);
}

PanoramaCache::BlockState PanoramaCache::state(PanoramaCache::BlockId id) const
{
    switch (id) {
    case FullRgb:   return stateInternal(m_rgb, true);
    case FullBw:    return stateInternal(m_bw, true);
    case ThumbRgb:  return stateInternal(m_rgb, false);
    case ThumbBw:   return stateInternal(m_bw, false);
    default: break;
    }
    return BlockState();
}

QImage PanoramaCache::ensureRgb32(const QImage &img)
{
    if (img.isNull()) return QImage();
    if (img.format() == QImage::Format_RGB32) return img;
    return img.convertToFormat(QImage::Format_RGB32);
}

QImage PanoramaCache::ensureBw8(const QImage &img)
{
    if (img.isNull()) return QImage();
    if (img.format() == QImage::Format_Indexed8) {
        QImage out = img;
        if (out.colorTable().isEmpty()) out.setColorTable(grayColorTableLocal());
        return out;
    }
    QImage out = img.convertToFormat(QImage::Format_Indexed8, grayColorTableLocal());
    out.setColorTable(grayColorTableLocal());
    return out;
}

double PanoramaCache::normalize360(double a)
{
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

void PanoramaCache::pushFrameInternal(PanoramaCache::Stream &s, const QImage &frame, bool isRgb, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg)
{
    if (frame.isNull() || frame.width() <= 0) return;

    int tileIndex = 0;
    int segments = 0;
    int sliceWThumb = 0;
    int thumbH = 0;
    QReadWriteLock *segLock = nullptr;
    bool writeFull = false;
    int sliceWFull = 0;
    int fullH = 0;

    {
        QReadLocker rl(&s.lock);
        if (!s.inited) {
            rl.unlock();
            QWriteLocker wl(&s.lock);
            if (!s.inited) reinitStream(s, frame.width(), isRgb);
        }
    }

    {
        QReadLocker rl(&s.lock);
        if (!s.inited || s.sliceWFull <= 0 || s.segments <= 0) return;
        segments = s.segments;
        if (angleDeg >= 0.0 && segments > 0) {
            const double a = normalize360(angleDeg);
            tileIndex = (int)(a / 360.0 * segments) % segments;
            if (tileIndex < 0) tileIndex = 0;
        } else {
            tileIndex = (fileIndex > 0 && segments > 0) ? (int)(fileIndex % (quint64)segments) : s.writeIndex;
        }
        if (!isRgb && segments > 0) tileIndex = (tileIndex + segments / 2) % segments;
        sliceWFull = s.sliceWFull;
        sliceWThumb = s.sliceWThumb;
        fullH = s.fullH;
        thumbH = s.thumbH;
        writeFull = s.fullReady && !s.full.isNull();
        segLock = (tileIndex >= 0 && tileIndex < s.segLocks.size()) ? s.segLocks[tileIndex].data() : nullptr;
    }

    if (sliceWFull <= 0 || sliceWThumb <= 0 || fullH <= 0 || thumbH <= 0) return;

    const int startXThumb = tileIndex * sliceWThumb;

    QImage thumbSlice = frame.scaled(sliceWThumb, thumbH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (isRgb) thumbSlice = ensureRgb32(thumbSlice);
    else thumbSlice = ensureBw8(thumbSlice);
    if (thumbSlice.isNull()) return;

    // overlap 必须为 0：原本的 alpha 混合会把每个 tile 左缘与"旧内容"（初始为黑）
    // 做渐变，导致每帧之间出现黑缝。每个 tile 直接整块写入，紧密相接。
    const int overlapFull = 0;
    const int overlapThumb = 0;
    QImage fullSlice;
    if (writeFull) {
        fullSlice = frame.scaled(sliceWFull, fullH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (isRgb) fullSlice = ensureRgb32(fullSlice);
        else fullSlice = ensureBw8(fullSlice);
        if (fullSlice.isNull()) writeFull = false;
    }

    {
        QReadLocker rl(&s.lock);
        if (!s.inited || s.thumb.isNull()) return;
        if (segLock) segLock->lockForWrite();
        if (isRgb) {
            if (writeFull) writeSliceRgb32(s.full, fullSlice, tileIndex * sliceWFull, overlapFull);
            writeSliceRgb32(s.thumb, thumbSlice, startXThumb, overlapThumb);
        } else {
            if (writeFull) writeSliceBw8(s.full, fullSlice, tileIndex * sliceWFull, overlapFull);
            writeSliceBw8(s.thumb, thumbSlice, startXThumb, overlapThumb);
        }
        if (segLock) segLock->unlock();
    }

    QWriteLocker l2(&s.lock);
    ++s.totalFrames;
    if (s.validFrames < s.segments) ++s.validFrames;
    if (tileIndex >= 0 && tileIndex < s.dirtyThumb.size()) s.dirtyThumb.setBit(tileIndex, true);
    if (fileIndex > 0 && tileIndex >= 0 && tileIndex < s.segments) {
        if (s.tileFileIdx.size() == s.segments) s.tileFileIdx[tileIndex] = fileIndex;
        if (!sourcePath.isEmpty() && s.tilePath.size() == s.segments) s.tilePath[tileIndex] = sourcePath;
        if (rxMs > 0 && s.tileRxMs.size() == s.segments) s.tileRxMs[tileIndex] = rxMs;
    }
    if (fileIndex > 0 && segments > 0) {
        s.writeIndex = (tileIndex + 1) % segments;
        if (s.validFrames >= s.segments) s.hasWrapped = true;
        return;
    }
    ++s.writeIndex;
    if (s.writeIndex >= s.segments) {
        s.writeIndex = 0;
        s.hasWrapped = true;
    }
}

QImage PanoramaCache::snapshotInternalThumb(const PanoramaCache::Stream &s) const
{
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (!s.inited || s.thumb.isNull() || s.segLocks.size() != s.segments) return QImage();
        segments = s.segments;
        sliceW = s.sliceWThumb;
        panoW = s.thumb.width();
        panoH = s.thumb.height();
        fmt = s.thumb.format();
    }

    QImage out(panoW, panoH, fmt);
    if (out.isNull()) return QImage();
    if (fmt == QImage::Format_Indexed8) out.setColorTable(grayColorTableLocal());

    QReadLocker sl(&s.lock);
    for (int tile = 0; tile < segments; ++tile) {
        const int startX = tile * sliceW;
        const int w = qMin(sliceW, panoW - startX);
        QReadWriteLock *lk = (tile >= 0 && tile < s.segLocks.size()) ? s.segLocks[tile].data() : nullptr;
        if (lk) lk->lockForRead();
        for (int y = 0; y < panoH; ++y) {
            const uchar *srcLine = s.thumb.constScanLine(y);
            uchar *dstLine = out.scanLine(y);
            const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;
            memcpy(dstLine + startX * bpp, srcLine + startX * bpp, w * bpp);
        }
        if (lk) lk->unlock();
    }
    return out;
}

QImage PanoramaCache::extractSliceInternal(const PanoramaCache::Stream &s, double angleDeg, bool allow180Fallback) const
{
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (!s.inited || !s.fullReady || s.full.isNull() || s.sliceWFull <= 0 || s.segments <= 0 || s.segLocks.size() != s.segments) return QImage();
        segments = s.segments;
        sliceW = s.sliceWFull;
        panoW = s.full.width();
        panoH = s.full.height();
        fmt = s.full.format();
    }

    QReadLocker sl(&s.lock);

    auto extractByTile = [&](int tileIndex)->QImage {
        if (tileIndex < 0 || tileIndex >= segments) return QImage();
        const int startX = tileIndex * sliceW;
        if (startX + sliceW > panoW) return QImage();

        QImage roi(sliceW, panoH, fmt);
        if (roi.isNull()) return QImage();
        if (roi.format() == QImage::Format_Indexed8) roi.setColorTable(grayColorTableLocal());

        const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;
        QReadWriteLock *lk = (tileIndex >= 0 && tileIndex < s.segLocks.size()) ? s.segLocks[tileIndex].data() : nullptr;
        if (lk) lk->lockForRead();
        for (int y = 0; y < panoH; ++y) {
            const uchar *srcLine = s.full.constScanLine(y);
            uchar *dstLine = roi.scanLine(y);
            memcpy(dstLine, srcLine + startX * bpp, sliceW * bpp);
        }
        if (lk) lk->unlock();
        return roi;
    };

    auto toTileIndex = [&](double a)->int {
        const double n = normalize360(a);
        int idx = (int)(n / 360.0 * segments);
        if (idx < 0) idx = 0;
        if (idx >= segments) idx = segments - 1;
        return idx;
    };

    const int tileA = toTileIndex(angleDeg);
    QImage roi = extractByTile(tileA);
    if (!allow180Fallback) return roi;
    const int tileB = toTileIndex(angleDeg + 180.0);
    if (tileB == tileA) return roi;
    if (!roi.isNull()) {
        if (roi.format() == QImage::Format_Indexed8) return roi.convertToFormat(QImage::Format_RGB32);
        return roi;
    }
    QImage roi2 = extractByTile(tileB);
    if (roi2.format() == QImage::Format_Indexed8) return roi2.convertToFormat(QImage::Format_RGB32);
    return roi2;
}

QImage PanoramaCache::extractThumbSliceInternal(const PanoramaCache::Stream &s, double angleDeg, bool allow180Fallback) const
{
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (!s.inited || s.thumb.isNull() || s.sliceWThumb <= 0 || s.segments <= 0 || s.segLocks.size() != s.segments) return QImage();
        segments = s.segments;
        sliceW = s.sliceWThumb;
        panoW = s.thumb.width();
        panoH = s.thumb.height();
        fmt = s.thumb.format();
    }

    QReadLocker sl(&s.lock);

    auto extractByTile = [&](int tileIndex)->QImage {
        if (tileIndex < 0 || tileIndex >= segments) return QImage();
        const int startX = tileIndex * sliceW;
        if (startX + sliceW > panoW) return QImage();

        QImage roi(sliceW, panoH, fmt);
        if (roi.isNull()) return QImage();
        if (roi.format() == QImage::Format_Indexed8) roi.setColorTable(grayColorTableLocal());

        const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;
        QReadWriteLock *lk = (tileIndex >= 0 && tileIndex < s.segLocks.size()) ? s.segLocks[tileIndex].data() : nullptr;
        if (lk) lk->lockForRead();
        for (int y = 0; y < panoH; ++y) {
            const uchar *srcLine = s.thumb.constScanLine(y);
            uchar *dstLine = roi.scanLine(y);
            memcpy(dstLine, srcLine + startX * bpp, sliceW * bpp);
        }
        if (lk) lk->unlock();
        return roi;
    };

    auto toTileIndex = [&](double a)->int {
        const double n = normalize360(a);
        int idx = (int)(n / 360.0 * segments);
        if (idx < 0) idx = 0;
        if (idx >= segments) idx = segments - 1;
        return idx;
    };

    const int tileA = toTileIndex(angleDeg);
    QImage roi = extractByTile(tileA);
    if (!allow180Fallback) return roi;
    const int tileB = toTileIndex(angleDeg + 180.0);
    if (tileB == tileA) return roi;
    if (!roi.isNull()) {
        if (roi.format() == QImage::Format_Indexed8) return roi.convertToFormat(QImage::Format_RGB32);
        return roi;
    }
    QImage roi2 = extractByTile(tileB);
    if (roi2.format() == QImage::Format_Indexed8) return roi2.convertToFormat(QImage::Format_RGB32);
    return roi2;
}

QImage PanoramaCache::extractWindowInternal(const PanoramaCache::Stream &s, bool full, double centerAngleDeg, int windowW) const
{
    int segments = 0;
    int sliceW = 0;
    int coverW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (full) {
            if (!s.inited || !s.fullReady || s.full.isNull() || s.sliceWFull <= 0 || s.segments <= 0 || s.segLocks.size() != s.segments) return QImage();
            sliceW = s.sliceWFull;
            panoH = s.full.height();
            fmt = s.full.format();
        } else {
            if (!s.inited || s.thumb.isNull() || s.sliceWThumb <= 0 || s.segments <= 0 || s.segLocks.size() != s.segments) return QImage();
            sliceW = s.sliceWThumb;
            panoH = s.thumb.height();
            fmt = s.thumb.format();
        }
        segments = s.segments;
        coverW = segments * sliceW;
    }

    if (coverW <= 0 || panoH <= 0 || sliceW <= 0) return QImage();
    if (windowW <= 0) windowW = sliceW;
    if (windowW > coverW) windowW = coverW;

    const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;

    // Map angle to pixel column inside the filled cylinder [0, coverW).
    const double a = normalize360(centerAngleDeg);
    int centerX = (int)(a / 360.0 * coverW);
    if (centerX < 0) centerX = 0;
    if (centerX >= coverW) centerX = coverW - 1;
    int startX = centerX - windowW / 2;
    startX %= coverW;
    if (startX < 0) startX += coverW;

    // Window may wrap around the 360 seam: split into [startX, startX+L1) and [0, L2).
    const int L1 = qMin(windowW, coverW - startX);
    const int L2 = windowW - L1;

    QImage out(windowW, panoH, fmt);
    if (out.isNull()) return QImage();
    if (fmt == QImage::Format_Indexed8) out.setColorTable(grayColorTableLocal());

    // Collect the tiles spanned by both segments, lock them ascending for read.
    QVector<int> tiles;
    auto addTiles = [&](int x0, int len) {
        if (len <= 0) return;
        const int t0 = x0 / sliceW;
        const int t1 = (x0 + len - 1) / sliceW;
        for (int t = t0; t <= t1 && t < segments; ++t)
            if (!tiles.contains(t)) tiles.push_back(t);
    };
    addTiles(startX, L1);
    if (L2 > 0) addTiles(0, L2);
    std::sort(tiles.begin(), tiles.end());

    QReadLocker sl(&s.lock);
    const QImage &srcImg = full ? s.full : s.thumb;
    if (srcImg.isNull()) return QImage();
    // Guard against a concurrent reinit having shrunk the backing image between
    // the metadata read above and this copy lock (avoids out-of-bounds reads).
    if (srcImg.width() < coverW || srcImg.height() < panoH) return QImage();

    for (int i = 0; i < tiles.size(); ++i) {
        const int t = tiles[i];
        QReadWriteLock *lk = (t >= 0 && t < s.segLocks.size()) ? s.segLocks[t].data() : nullptr;
        if (lk) lk->lockForRead();
    }
    for (int y = 0; y < panoH; ++y) {
        const uchar *srcLine = srcImg.constScanLine(y);
        uchar *dstLine = out.scanLine(y);
        memcpy(dstLine, srcLine + (qint64)startX * bpp, (qint64)L1 * bpp);
        if (L2 > 0) memcpy(dstLine + (qint64)L1 * bpp, srcLine, (qint64)L2 * bpp);
    }
    for (int i = tiles.size() - 1; i >= 0; --i) {
        const int t = tiles[i];
        QReadWriteLock *lk = (t >= 0 && t < s.segLocks.size()) ? s.segLocks[t].data() : nullptr;
        if (lk) lk->unlock();
    }
    return out;
}

PanoramaCache::ThumbInfo PanoramaCache::thumbInfoInternal(const PanoramaCache::Stream &s) const
{
    QReadLocker l(&s.lock);
    ThumbInfo info;
    info.inited = s.inited && !s.thumb.isNull() && (s.segLocks.size() == s.segments) && s.segments > 0 && s.sliceWThumb > 0;
    if (!info.inited) return info;
    info.panoW = s.thumb.width();
    info.panoH = s.thumb.height();
    info.segments = s.segments;
    info.sliceW = s.sliceWThumb;
    info.format = s.thumb.format();
    return info;
}

QVector<int> PanoramaCache::takeDirtyThumbTilesInternal(PanoramaCache::Stream &s)
{
    QWriteLocker l(&s.lock);
    if (!s.inited || s.thumb.isNull() || s.segments <= 0 || s.segLocks.size() != s.segments) return {};
    if (s.dirtyThumb.size() != s.segments) s.dirtyThumb = QBitArray(s.segments, true);
    QVector<int> tiles;
    tiles.reserve(s.segments);
    for (int i = 0; i < s.segments; ++i) {
        if (!s.dirtyThumb.testBit(i)) continue;
        tiles.push_back(i);
        s.dirtyThumb.setBit(i, false);
    }
    return tiles;
}

bool PanoramaCache::blitThumbTileInternal(const PanoramaCache::Stream &s, int tileIndex, QImage &dst) const
{
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (!s.inited || s.thumb.isNull() || s.segments <= 0 || s.segLocks.size() != s.segments) return false;
        segments = s.segments;
        sliceW = s.sliceWThumb;
        panoW = s.thumb.width();
        panoH = s.thumb.height();
        fmt = s.thumb.format();
    }

    if (tileIndex < 0 || tileIndex >= segments) return false;
    if (dst.isNull() || dst.width() != panoW || dst.height() != panoH || dst.format() != fmt) return false;
    if (fmt == QImage::Format_Indexed8 && dst.colorTable().isEmpty()) dst.setColorTable(grayColorTableLocal());

    QReadLocker sl(&s.lock);
    const int startX = tileIndex * sliceW;
    const int w = qMin(sliceW, panoW - startX);
    QReadWriteLock *lk = (tileIndex >= 0 && tileIndex < s.segLocks.size()) ? s.segLocks[tileIndex].data() : nullptr;
    if (lk) lk->lockForRead();
    const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;
    for (int y = 0; y < panoH; ++y) {
        const uchar *srcLine = s.thumb.constScanLine(y);
        uchar *dstLine = dst.scanLine(y);
        memcpy(dstLine + startX * bpp, srcLine + startX * bpp, w * bpp);
    }
    if (lk) lk->unlock();
    return true;
}

bool PanoramaCache::copyFullTileInternal(const PanoramaCache::Stream &s, int tileIndex, QImage &out) const
{
    int segments = 0;
    int sliceW = 0;
    int panoW = 0;
    int panoH = 0;
    QImage::Format fmt = QImage::Format_Invalid;
    {
        QReadLocker l(&s.lock);
        if (!s.inited || !s.fullReady || s.full.isNull() || s.segments <= 0 || s.sliceWFull <= 0 || s.segLocks.size() != s.segments) return false;
        segments = s.segments;
        sliceW = s.sliceWFull;
        panoW = s.full.width();
        panoH = s.full.height();
        fmt = s.full.format();
    }

    if (tileIndex < 0 || tileIndex >= segments) return false;
    const int startX = tileIndex * sliceW;
    if (startX + sliceW > panoW) return false;

    QImage roi(sliceW, panoH, fmt);
    if (roi.isNull()) return false;
    if (fmt == QImage::Format_Indexed8) roi.setColorTable(grayColorTableLocal());

    QReadLocker sl(&s.lock);
    QReadWriteLock *lk = (tileIndex >= 0 && tileIndex < s.segLocks.size()) ? s.segLocks[tileIndex].data() : nullptr;
    if (lk) lk->lockForRead();
    const int bpp = (fmt == QImage::Format_Indexed8) ? 1 : 4;
    for (int y = 0; y < panoH; ++y) {
        const uchar *srcLine = s.full.constScanLine(y);
        uchar *dstLine = roi.scanLine(y);
        memcpy(dstLine, srcLine + startX * bpp, sliceW * bpp);
    }
    if (lk) lk->unlock();
    out = roi;
    return true;
}

bool PanoramaCache::snapshotFullTileSourcesInternal(const PanoramaCache::Stream &s, QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const
{
    QReadLocker l(&s.lock);
    if (!s.inited || !s.fullReady || s.full.isNull() || s.segments <= 0) return false;
    if (s.tileFileIdx.size() != s.segments || s.tilePath.size() != s.segments || s.tileRxMs.size() != s.segments) return false;
    fileIdx = s.tileFileIdx;
    paths = s.tilePath;
    rxMs = s.tileRxMs;
    return true;
}

PanoramaCache::BlockState PanoramaCache::stateInternal(const PanoramaCache::Stream &s, bool full) const
{
    QReadLocker l(&s.lock);
    BlockState st;
    st.inited = full ? (s.inited && s.fullReady) : s.inited;
    st.panoW = full ? (s.fullReady ? s.fullW : 0) : s.thumbW;
    st.panoH = full ? (s.fullReady ? s.fullH : 0) : s.thumbH;
    st.segments = s.segments;
    st.sliceW = full ? s.sliceWFull : s.sliceWThumb;
    st.writeIndex = s.writeIndex;
    st.validFrames = s.validFrames;
    st.hasWrapped = s.hasWrapped;
    st.totalFrames = s.totalFrames;
    st.droppedFrames = s.droppedFrames;
    return st;
}

void PanoramaCache::reinitStream(PanoramaCache::Stream &s, int frameW, bool isRgb)
{
    const int fullW = s.fullW;
    const int fullH = s.fullH;
    const int thumbW = s.thumbW;
    const int thumbH = s.thumbH;

    int segByImg = (frameW > 0) ? qMax(1, fullW / frameW) : 1;
    const int segments = qBound(8, segByImg, 1024);
    const int sliceWFull = qMax(1, fullW / segments);
    const int sliceWThumb = qMax(1, thumbW / segments);

    QImage full;
    QImage thumb;
    if (isRgb) {
        thumb = QImage(thumbW, thumbH, QImage::Format_RGB32);
        if (!thumb.isNull()) thumb.fill(Qt::black);
        full = QImage(fullW, fullH, QImage::Format_RGB32);
        if (!full.isNull()) full.fill(Qt::black);
    } else {
        thumb = QImage(thumbW, thumbH, QImage::Format_Indexed8);
        if (!thumb.isNull()) {
            thumb.setColorTable(grayColorTableLocal());
            thumb.fill(0);
        }
        full = QImage(fullW, fullH, QImage::Format_Indexed8);
        if (!full.isNull()) {
            full.setColorTable(grayColorTableLocal());
            full.fill(0);
        }
    }

    if (thumb.isNull()) {
        s.inited = false;
        s.fullReady = false;
        return;
    }

    s.segLocks.clear();
    s.segLocks.reserve(segments);
    for (int i = 0; i < segments; ++i) s.segLocks.push_back(QSharedPointer<QReadWriteLock>(new QReadWriteLock()));

    s.dirtyThumb = QBitArray(segments, true);
    s.tileFileIdx = QVector<quint64>(segments, 0);
    s.tilePath = QVector<QString>(segments, QString());
    s.tileRxMs = QVector<qint64>(segments, 0);
    s.full = full;
    s.thumb = thumb;
    s.segments = segments;
    s.sliceWFull = sliceWFull;
    s.sliceWThumb = sliceWThumb;
    s.writeIndex = 0;
    s.validFrames = 0;
    s.hasWrapped = false;
    s.totalFrames = 0;
    s.droppedFrames = 0;
    s.fullReady = !s.full.isNull();
    s.inited = true;
}

static bool writeBmp24Header(QFile &f, int fullW, int fullH, qint64 *pixelOffset, QString *errMsg)
{
    const int bytesPerPixel = 3;
    const int rowStride = fullW * bytesPerPixel;
    const int off = 54;
    const qint64 imageSize = (qint64)rowStride * fullH;
    const qint64 fileSize = off + imageSize;
    if (!f.open(QIODevice::ReadWrite)) {
        if (errMsg) *errMsg = "Cannot create file: " + f.fileName();
        return false;
    }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0x4D42);
    ds << quint32((quint32)fileSize);
    ds << quint16(0) << quint16(0);
    ds << quint32(off);
    ds << quint32(40);
    ds << qint32(fullW);
    ds << qint32(fullH);
    ds << quint16(1);
    ds << quint16(24);
    ds << quint32(0);
    ds << quint32((quint32)imageSize);
    ds << qint32(0);
    ds << qint32(0);
    ds << quint32(0);
    ds << quint32(0);
    if (pixelOffset) *pixelOffset = off;
    return true;
}

static bool writeBmp8Header(QFile &f, int fullW, int fullH, qint64 *pixelOffset, QString *errMsg)
{
    const int bytesPerPixel = 1;
    const int rowStride = fullW * bytesPerPixel;
    const int paletteBytes = 256 * 4;
    const int off = 14 + 40 + paletteBytes;
    const qint64 imageSize = (qint64)rowStride * fullH;
    const qint64 fileSize = off + imageSize;
    if (!f.open(QIODevice::ReadWrite)) {
        if (errMsg) *errMsg = "Cannot create file: " + f.fileName();
        return false;
    }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0x4D42);
    ds << quint32((quint32)fileSize);
    ds << quint16(0) << quint16(0);
    ds << quint32(off);
    ds << quint32(40);
    ds << qint32(fullW);
    ds << qint32(fullH);
    ds << quint16(1);
    ds << quint16(8);
    ds << quint32(0);
    ds << quint32((quint32)imageSize);
    ds << qint32(0);
    ds << qint32(0);
    ds << quint32(256);
    ds << quint32(0);
    for (int i = 0; i < 256; ++i) {
        ds << quint8(i) << quint8(i) << quint8(i) << quint8(0);
    }
    if (pixelOffset) *pixelOffset = off;
    return true;
}

bool PanoramaCache::saveFullPanoramaBmp(const QString &rgbPath, const QString &bwPath, QString *errMsg) const
{
    QImage fullRgb;
    QImage fullBw;
    QVector<QSharedPointer<QReadWriteLock>> locksR;
    QVector<QSharedPointer<QReadWriteLock>> locksB;
    int segR = 0, segB = 0;
    int sliceWR = 0, sliceWB = 0;

    {
        QReadLocker lr(&m_rgb.lock);
        if (!m_rgb.inited || m_rgb.full.isNull() || m_rgb.segments <= 0 || m_rgb.sliceWFull <= 0 || m_rgb.segLocks.size() != m_rgb.segments) {
            if (errMsg) *errMsg = "RGB snapshot empty";
            return false;
        }
        fullRgb = m_rgb.full;
        locksR = m_rgb.segLocks;
        segR = m_rgb.segments;
        sliceWR = m_rgb.sliceWFull;
    }
    {
        QReadLocker lb(&m_bw.lock);
        if (!m_bw.inited || m_bw.full.isNull() || m_bw.segments <= 0 || m_bw.sliceWFull <= 0 || m_bw.segLocks.size() != m_bw.segments) {
            if (errMsg) *errMsg = "BW snapshot empty";
            return false;
        }
        fullBw = m_bw.full;
        locksB = m_bw.segLocks;
        segB = m_bw.segments;
        sliceWB = m_bw.sliceWFull;
    }

    const int fullW = fullRgb.width();
    const int fullH = fullRgb.height();
    if (fullW <= 0 || fullH <= 0 || fullBw.width() != fullW || fullBw.height() != fullH) {
        if (errMsg) *errMsg = "Panorama size mismatch";
        return false;
    }

    QFile fr(rgbPath);
    QFile fb(bwPath);
    qint64 offRgb = 0;
    qint64 offBw = 0;
    if (!writeBmp24Header(fr, fullW, fullH, &offRgb, errMsg)) return false;
    if (!writeBmp8Header(fb, fullW, fullH, &offBw, errMsg)) { fr.close(); return false; }

    const int rowStrideRgb = fullW * 3;
    const int rowStrideBw = fullW;

    for (int tile = 0; tile < segR; ++tile) {
        const int startX = tile * sliceWR;
        const int w = qMin(sliceWR, fullW - startX);
        QReadWriteLock *lk = (tile >= 0 && tile < locksR.size()) ? locksR[tile].data() : nullptr;
        if (lk) lk->lockForRead();
        QByteArray rowBuf;
        rowBuf.resize(w * 3);
        for (int y = 0; y < fullH; ++y) {
            const qint64 pos = offRgb + (qint64)(fullH - 1 - y) * rowStrideRgb + (qint64)startX * 3;
            if (!fr.seek(pos)) { if (errMsg) *errMsg = "RGB seek failed"; fr.close(); fb.close(); return false; }
            const QRgb *src = reinterpret_cast<const QRgb*>(fullRgb.constScanLine(y)) + startX;
            uchar *dst = reinterpret_cast<uchar*>(rowBuf.data());
            for (int x = 0; x < w; ++x) {
                const QRgb p = src[x];
                dst[x * 3 + 0] = (uchar)((p >> 16) & 0xFF);
                dst[x * 3 + 1] = (uchar)((p >> 8) & 0xFF);
                dst[x * 3 + 2] = (uchar)(p & 0xFF);
            }
            const qint64 need = (qint64)w * 3;
            if (fr.write(rowBuf.constData(), need) != need) { if (errMsg) *errMsg = "RGB write failed"; fr.close(); fb.close(); return false; }
        }
        if (lk) lk->unlock();
    }

    for (int tile = 0; tile < segB; ++tile) {
        const int startX = tile * sliceWB;
        const int w = qMin(sliceWB, fullW - startX);
        QReadWriteLock *lk = (tile >= 0 && tile < locksB.size()) ? locksB[tile].data() : nullptr;
        if (lk) lk->lockForRead();
        for (int y = 0; y < fullH; ++y) {
            const qint64 pos = offBw + (qint64)(fullH - 1 - y) * rowStrideBw + (qint64)startX;
            if (!fb.seek(pos)) { if (errMsg) *errMsg = "BW seek failed"; fr.close(); fb.close(); return false; }
            const uchar *line = fullBw.constScanLine(y) + startX;
            const qint64 need = (qint64)w;
            if (fb.write((const char*)line, need) != need) { if (errMsg) *errMsg = "BW write failed"; fr.close(); fb.close(); return false; }
        }
        if (lk) lk->unlock();
    }

    fr.close();
    fb.close();
    return true;
}

void PanoramaCache::writeSliceRgb32(QImage &dst, const QImage &slice, int startX, int overlap)
{
    if (dst.isNull() || slice.isNull()) return;
    const int panoW = dst.width();
    const int panoH = dst.height();
    const int sliceW = slice.width();
    if (startX < 0 || startX + sliceW > panoW) return;
    if (slice.height() != panoH) return;

    for (int y = 0; y < panoH; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb*>(dst.scanLine(y)) + startX;
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(slice.constScanLine(y));
        if (overlap > 0 && sliceW > overlap) {
            for (int x = 0; x < overlap; ++x) {
                const int a = (x * 255) / overlap;
                const int inv = 255 - a;
                const QRgb od = dstLine[x];
                const QRgb nd = srcLine[x];
                const int or_ = (od >> 16) & 0xFF;
                const int og  = (od >> 8) & 0xFF;
                const int ob  = od & 0xFF;
                const int nr_ = (nd >> 16) & 0xFF;
                const int ng  = (nd >> 8) & 0xFF;
                const int nb  = nd & 0xFF;
                const int r = (or_ * inv + nr_ * a + 127) / 255;
                const int g = (og * inv + ng * a + 127) / 255;
                const int b = (ob * inv + nb * a + 127) / 255;
                dstLine[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
            memcpy(dstLine + overlap, srcLine + overlap, (sliceW - overlap) * 4);
        } else {
            memcpy(dstLine, srcLine, sliceW * 4);
        }
    }
}

void PanoramaCache::writeSliceBw8(QImage &dst, const QImage &slice, int startX, int overlap)
{
    if (dst.isNull() || slice.isNull()) return;
    const int panoW = dst.width();
    const int panoH = dst.height();
    const int sliceW = slice.width();
    if (startX < 0 || startX + sliceW > panoW) return;
    if (slice.height() != panoH) return;

    for (int y = 0; y < panoH; ++y) {
        uchar *dstLine = dst.scanLine(y) + startX;
        const uchar *srcLine = slice.constScanLine(y);
        if (overlap > 0 && sliceW > overlap) {
            for (int x = 0; x < overlap; ++x) {
                const int a = (x * 255) / overlap;
                const int inv = 255 - a;
                dstLine[x] = (uchar)((dstLine[x] * inv + srcLine[x] * a + 127) / 255);
            }
            memcpy(dstLine + overlap, srcLine + overlap, sliceW - overlap);
        } else {
            memcpy(dstLine, srcLine, sliceW);
        }
    }
}

