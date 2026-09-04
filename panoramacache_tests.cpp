#include <QtTest/QtTest>
#include <QThread>
#include <QAtomicInteger>

#include "panoramacache.h"

static QImage solidRgb(int w, int h, QRgb c)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    return img;
}

static bool regionAllColor(const QImage &img, int x0, int w, QRgb c)
{
    if (img.isNull()) return false;
    if (img.format() != QImage::Format_RGB32) return false;
    if (x0 < 0 || w <= 0 || x0 + w > img.width()) return false;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = x0; x < x0 + w; ++x) {
            if (line[x] != c) return false;
        }
    }
    return true;
}

static bool rowAllColor(const QImage &img, int y, QRgb color)
{
    if (img.isNull() || img.format() != QImage::Format_RGB32 || y < 0 || y >= img.height()) {
        return false;
    }
    const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = 0; x < img.width(); ++x) {
        if (line[x] != color) return false;
    }
    return true;
}

class PanoramaCacheTests : public QObject
{
    Q_OBJECT
private slots:
    void firstFrameBaseline()
    {
        PanoramaCache cache;
        cache.configureFullSize(64, 4);
        cache.configureThumbSize(64, 4);

        cache.pushRgbFrame(solidRgb(8, 4, qRgb(255, 0, 0)));
        cache.pushRgbFrame(solidRgb(8, 4, qRgb(0, 255, 0)));

        const QImage thumb = cache.snapshotThumbRgb();
        QVERIFY(thumb.size() == QSize(64, 4));
        QVERIFY(regionAllColor(thumb, 0, 8, qRgb(255, 0, 0)));
        QVERIFY(regionAllColor(thumb, 8, 8, qRgb(0, 255, 0)));
    }

    void circularOverwriteBoundary()
    {
        PanoramaCache cache;
        cache.configureFullSize(64, 4);
        cache.configureThumbSize(64, 4);

        const QVector<QRgb> colors = {
            qRgb(255, 0, 0),
            qRgb(0, 255, 0),
            qRgb(0, 0, 255),
            qRgb(255, 255, 0),
            qRgb(255, 0, 255),
            qRgb(0, 255, 255),
            qRgb(128, 128, 128),
            qRgb(255, 128, 0),
            qRgb(10, 20, 30),
            qRgb(40, 50, 60)
        };

        for (int i = 0; i < 10; ++i) {
            cache.pushRgbFrame(solidRgb(8, 4, colors[i]));
        }

        const PanoramaCache::BlockState st = cache.state(PanoramaCache::ThumbRgb);
        QVERIFY(st.inited);
        QCOMPARE(st.segments, 8);
        QCOMPARE(st.validFrames, 8);
        QVERIFY(st.hasWrapped);
        QCOMPARE(st.writeIndex, 2);

        const QImage thumb = cache.snapshotThumbRgb();
        QVERIFY(regionAllColor(thumb, 0, 8, colors[8]));
        QVERIFY(regionAllColor(thumb, 8, 8, colors[9]));
    }

    void concurrentReadWrite()
    {
        PanoramaCache cache;
        cache.configureFullSize(64, 4);
        cache.configureThumbSize(64, 4);

        QAtomicInteger<int> running(1);
        QAtomicInteger<int> pushed(0);
        QAtomicInteger<int> readOk(0);

        QThread writer;
        QThread reader;

        QObject writerObj;
        QObject readerObj;

        writerObj.moveToThread(&writer);
        readerObj.moveToThread(&reader);

        QObject::connect(&writer, &QThread::started, &writerObj, [&]() {
            int i = 0;
            while (running.loadAcquire() != 0) {
                cache.pushRgbFrame(solidRgb(8, 4, qRgb(i & 0xFF, (i * 3) & 0xFF, (i * 7) & 0xFF)));
                ++pushed;
                ++i;
            }
        });
        QObject::connect(&reader, &QThread::started, &readerObj, [&]() {
            while (running.loadAcquire() != 0) {
                QImage img = cache.snapshotThumbRgb();
                if (!img.isNull() && img.size() == QSize(64, 4)) {
                    ++readOk;
                }
            }
        });

        writer.start();
        reader.start();
        QTest::qWait(200);
        running.storeRelease(0);
        writer.quit();
        reader.quit();
        writer.wait();
        reader.wait();

        QVERIFY(pushed.loadAcquire() > 0);
        QVERIFY(readOk.loadAcquire() > 0);
        const PanoramaCache::BlockState st = cache.state(PanoramaCache::ThumbRgb);
        QVERIFY(st.validFrames <= st.segments);
    }

    void targetCropWrapsAcrossPanoramaEdges()
    {
        PanoramaCache cache;
        cache.configureFullSize(64, 8);
        cache.configureThumbSize(64, 8);

        const QRgb red = qRgb(255, 0, 0);
        const QRgb green = qRgb(0, 255, 0);
        const QRgb orange = qRgb(255, 128, 0);
        const QVector<QRgb> colors = {
            red, green, qRgb(0, 0, 255), qRgb(255, 255, 0),
            qRgb(255, 0, 255), qRgb(0, 255, 255), qRgb(128, 128, 128), orange
        };
        for (QRgb color : colors) cache.pushRgbFrame(solidRgb(8, 8, color));

        const QImage leftEdge = cache.extractFullRgbCrop(2, 4, 8);
        QVERIFY(regionAllColor(leftEdge, 0, 2, orange));
        QVERIFY(regionAllColor(leftEdge, 2, 6, red));

        const QImage rightEdge = cache.extractFullRgbCrop(62, 4, 8);
        QVERIFY(regionAllColor(rightEdge, 0, 6, orange));
        QVERIFY(regionAllColor(rightEdge, 6, 2, red));

        const QImage tileBoundary = cache.extractFullRgbCrop(8, 4, 8);
        QVERIFY(regionAllColor(tileBoundary, 0, 4, red));
        QVERIFY(regionAllColor(tileBoundary, 4, 4, green));
    }

    void targetCropPadsOnlyVertically()
    {
        PanoramaCache cache;
        cache.configureFullSize(64, 8);
        cache.configureThumbSize(64, 8);
        const QRgb red = qRgb(255, 0, 0);
        for (int i = 0; i < 8; ++i) cache.pushRgbFrame(solidRgb(8, 8, red));

        const QImage crop = cache.extractFullRgbCrop(0, 1, 4);
        QCOMPARE(crop.size(), QSize(4, 4));
        QVERIFY(rowAllColor(crop, 0, qRgb(0, 0, 0)));
        QVERIFY(rowAllColor(crop, 1, red));
        QVERIFY(rowAllColor(crop, 2, red));
        QVERIFY(rowAllColor(crop, 3, red));
    }
};

QTEST_MAIN(PanoramaCacheTests)
#include "panoramacache_tests.moc"
