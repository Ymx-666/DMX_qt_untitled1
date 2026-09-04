#ifndef TARGETRECORD_H
#define TARGETRECORD_H

#include <QDateTime>
#include <QString>
#include <QtGlobal>

struct TargetRecord
{
    QString id;
    QDateTime time;
    QString imagePath;
    QString stream;
    QString className;
    QString lastStream;
    QString frameKey;
    QString state;
    QDateTime firstTime;
    QDateTime lastTime;
    double azimuthDeg = 0.0;
    double elevationAngleDeg = 0.0;
    bool hasElevationAngle = false;
    double confidence = 0.0;
    double classConfidence = 0.0;
    double score = 0.0;
    double trackAgeSec = 0.0;
    int hits = 1;
    int panoX = 0;
    int panoY = 0;
    int frameX = 0;
    int frameY = 0;
    bool hasRoiBox = false;
    int roiBoxX1 = 0;
    int roiBoxY1 = 0;
    int roiBoxX2 = 0;
    int roiBoxY2 = 0;
    double boxAspect = 0.0;
    double boxArea = 0.0;
    double imageConfidence = 0.0;
    quint64 appearanceHash = 0;
    bool hasAppearanceHash = false;
    bool staticSuppressed = false;
    int staticTrackId = 0;
    int staticStableHits = 0;
    double staticContextScore = 0.0;
};

#endif // TARGETRECORD_H
