#ifndef POLARPANORAMAPROJECTOR_H
#define POLARPANORAMAPROJECTOR_H

#include <QImage>

class PolarPanoramaProjector
{
public:
    static QImage rotateAzimuth180(const QImage &src);
    static QImage projectHeightPolar(const QImage &panorama,
                                     int size,
                                     int innerRadius,
                                     int outerRadius);
};

#endif // POLARPANORAMAPROJECTOR_H
