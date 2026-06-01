#ifndef PANORAMACACHE_H
#define PANORAMACACHE_H

#include <QImage>
#include <QReadWriteLock>
#include <QAtomicInteger>
#include <QVector>
#include <QSharedPointer>
#include <QBitArray>
#include <QString>

class PanoramaCache
{
public:
    enum BlockId {
        FullRgb = 0,
        FullBw = 1,
        ThumbRgb = 2,
        ThumbBw = 3
    };

    struct BlockState {
        bool inited = false;
        int panoW = 0;
        int panoH = 0;
        int segments = 0;
        int sliceW = 0;
        int writeIndex = 0;
        int validFrames = 0;
        bool hasWrapped = false;
        quint64 totalFrames = 0;
        quint64 droppedFrames = 0;
    };

    PanoramaCache();
    ~PanoramaCache();

    void configureFullSize(int w, int h);
    void configureThumbSize(int w, int h);

    void resetRgb();
    void resetBw();
    void resetAll();

    void pushRgbFrame(const QImage &frameRgb32);
    void pushBwFrame(const QImage &frameBwIndexed8);
    void pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex);
    void pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex);
    void pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex, const QString &sourcePath, qint64 rxMs);
    void pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex, const QString &sourcePath, qint64 rxMs);
    void pushRgbFrame(const QImage &frameRgb32, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg);
    void pushBwFrame(const QImage &frameBwIndexed8, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg);

    QImage snapshotThumbRgb() const;
    QImage snapshotThumbBw() const;

    QImage extractFullRgbSliceByAngle(double angleDeg) const;
    QImage extractFullBwSliceByAngle(double angleDeg, bool allow180Fallback) const;
    QImage extractThumbRgbSliceByAngle(double angleDeg) const;
    QImage extractThumbBwSliceByAngle(double angleDeg, bool allow180Fallback) const;

    // Sliding-window extraction: take a windowW-pixel-wide column window centered
    // at centerAngleDeg, NOT aligned to tile boundaries, with 360 wraparound.
    // windowW <= 0 means "one tile width" (sliceW) for that stream/resolution.
    QImage extractFullRgbWindow(double centerAngleDeg, int windowW) const;
    QImage extractFullBwWindow(double centerAngleDeg, int windowW) const;
    QImage extractThumbRgbWindow(double centerAngleDeg, int windowW) const;
    QImage extractThumbBwWindow(double centerAngleDeg, int windowW) const;

    bool copyFullRgbTile(int tileIndex, QImage &out) const;
    bool copyFullBwTile(int tileIndex, QImage &out) const;
    bool snapshotFullRgbTileSources(QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const;
    bool snapshotFullBwTileSources(QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const;

    struct ThumbInfo {
        bool inited = false;
        int panoW = 0;
        int panoH = 0;
        int segments = 0;
        int sliceW = 0;
        QImage::Format format = QImage::Format_Invalid;
    };
    ThumbInfo thumbInfoRgb() const;
    ThumbInfo thumbInfoBw() const;

    QVector<int> takeDirtyThumbRgbTiles();
    QVector<int> takeDirtyThumbBwTiles();
    bool blitThumbRgbTileTo(int tileIndex, QImage &dst) const;
    bool blitThumbBwTileTo(int tileIndex, QImage &dst) const;

    BlockState state(BlockId id) const;

private:
    struct Stream {
        mutable QReadWriteLock lock;
        QImage full;
        QImage thumb;
        QVector<QSharedPointer<QReadWriteLock>> segLocks;
        QBitArray dirtyThumb;
        QVector<quint64> tileFileIdx;
        QVector<QString> tilePath;
        QVector<qint64> tileRxMs;
        int fullW = 65536;
        int fullH = 4096;
        int thumbW = 8192;
        int thumbH = 240;
        int segments = 0;
        int sliceWFull = 0;
        int sliceWThumb = 0;
        int writeIndex = 0;
        int validFrames = 0;
        bool hasWrapped = false;
        quint64 totalFrames = 0;
        quint64 droppedFrames = 0;
        bool fullReady = false;
        bool inited = false;
    };

    static QImage ensureRgb32(const QImage &img);
    static QImage ensureBw8(const QImage &img);
    static double normalize360(double a);

    void pushFrameInternal(Stream &s, const QImage &frame, bool isRgb, quint64 fileIndex, const QString &sourcePath, qint64 rxMs, double angleDeg);
    QImage snapshotInternalThumb(const Stream &s) const;
    QImage extractSliceInternal(const Stream &s, double angleDeg, bool allow180Fallback) const;
    QImage extractThumbSliceInternal(const Stream &s, double angleDeg, bool allow180Fallback) const;
    QImage extractWindowInternal(const Stream &s, bool full, double centerAngleDeg, int windowW) const;
    BlockState stateInternal(const Stream &s, bool full) const;
    ThumbInfo thumbInfoInternal(const Stream &s) const;
    QVector<int> takeDirtyThumbTilesInternal(Stream &s);
    bool blitThumbTileInternal(const Stream &s, int tileIndex, QImage &dst) const;
    bool copyFullTileInternal(const Stream &s, int tileIndex, QImage &out) const;
    bool snapshotFullTileSourcesInternal(const Stream &s, QVector<quint64> &fileIdx, QVector<QString> &paths, QVector<qint64> &rxMs) const;

    void reinitStream(Stream &s, int frameW, bool isRgb);
    void writeSliceRgb32(QImage &dst, const QImage &slice, int startX, int overlap);
    void writeSliceBw8(QImage &dst, const QImage &slice, int startX, int overlap);

    Stream m_rgb;
    Stream m_bw;
};

#endif // PANORAMACACHE_H
