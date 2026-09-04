#include "polarpanoramaprojector.h"

#include <QtMath>

QImage PolarPanoramaProjector::rotateAzimuth180(const QImage &src)
{
    if (src.isNull() || src.width() <= 1) return src;
    QImage in = src.convertToFormat(QImage::Format_RGB32);
    QImage out(in.size(), QImage::Format_RGB32);
    if (out.isNull()) return QImage();

    const int w = in.width();
    const int h = in.height();
    const int shift = w / 2;
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) d[(x + shift) % w] = s[x];
    }
    return out;
}

QImage PolarPanoramaProjector::projectHeightPolar(const QImage &panorama,
                                                  int size,
                                                  int innerRadius,
                                                  int outerRadius)
{
    if (panorama.isNull() || size <= 0 || innerRadius < 0 || outerRadius <= innerRadius)
        return QImage();

    QImage src = panorama.convertToFormat(QImage::Format_RGB32);
    QImage out(size, size, QImage::Format_RGB32);
    if (out.isNull()) return QImage();
    out.fill(qRgb(8, 10, 12));

    const int cx = size / 2;
    const int cy = size / 2;
    const int sw = src.width();
    const int sh = src.height();
    const double twoPi = 2.0 * M_PI;
    const double radialSpan = qMax(1, outerRadius - innerRadius);

    for (int y = 0; y < size; ++y) {
        QRgb *dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        const double dy = (double)cy - (double)y;
        for (int x = 0; x < size; ++x) {
            const double dx = (double)x - (double)cx;
            const double r = qSqrt(dx * dx + dy * dy);
            if (r < innerRadius || r > outerRadius) continue;

            double theta = qAtan2(dx, dy); // north=0, clockwise positive.
            if (theta < 0.0) theta += twoPi;
            int sx = (int)(theta / twoPi * (double)sw);
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;

            // Radius is sky height: outer ring samples image top/high sky,
            // inner ring samples low horizon/ground.
            int sy = (int)((double)(outerRadius - r) / radialSpan * (double)(sh - 1));
            if (sy < 0) sy = 0;
            if (sy >= sh) sy = sh - 1;

            const QRgb c = reinterpret_cast<const QRgb*>(src.constScanLine(sy))[sx];
            const int rr = (qRed(c) * 82 + 5 * 18) / 100;
            const int gg = (qGreen(c) * 82 + 14 * 18) / 100;
            const int bb = (qBlue(c) * 82 + 18 * 18) / 100;
            dst[x] = qRgb(rr, gg, bb);
        }
    }
    return out;
}
