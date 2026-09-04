#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QStringList>
#include <QtGlobal>

class AppConfig
{
public:
    AppConfig();

    static const AppConfig &instance();
    static AppConfig loadFile(const QString &path, bool applyEnvironment);
    static AppConfig loadDefault(bool applyEnvironment);

    QString loadedPath;
    QString loadError;
    bool loadedFromFile;

    QString dataRoot;
    QString saveRoot;
    QString recordRoot;
    QString logRoot;
    QString rawLogRoot;
    QString shareMount;

    bool angleLookup;
    qint64 captureLatencyMs;
    qint64 captureLatencyRgbMs;
    qint64 captureLatencyBwMs;
    bool uiDropStale;
    int uiQueueCap;

    int fullWidth;
    int fullHeight;
    double cameraVerticalFovDeg;
    int thumbWidth;
    int thumbHeight;
    int previewWidth;
    int previewJpegQuality;
    bool saveRawTiles;

    int recordRollMinutes;

    QString deviceIp;
    quint16 cmdPortSend;
    quint16 cmdPortReply;
    quint16 pathPort;
    bool replayMode;

    QString turntableSerialPort;
    int turntableBaudRate;
    QString turntableDirection;
    int turntableSpeed;
    bool turntableOrthoEnabled;
    int turntableOrthoLength;
    bool turntableFeedbackEnabled;

    bool detectEnabled;
    QString detectStream;
    QString detectSaveRoot;
    QString detectSkyMaskSaveRoot;
    int detectCropSize;
    int detectBackgroundFrames;
    int detectSkyMargin;
    int detectSkyShrinkPixels;
    int detectMaxCandidatesPerFrame;
    int detectMinArea;
    int detectMaxArea;
    int detectMedianKernel;
    int detectTophatKernel;
    double detectThresholdK;
    int detectMinContrast;
    int detectNmsRadius;
    int detectFeatureLimit;
    int detectFeatureNmsRadius;
    int detectJpegQuality;
    QString detectReferenceTemplatePath;

    bool detectYoloEnabled;
    QString detectYoloModelPath;
    QString detectYoloMode;
    QStringList detectYoloClassNames;
    int detectYoloInputSize;
    double detectYoloConfThreshold;
    double detectYoloConfirmThreshold;
    double detectYoloSupplementThreshold;
    double detectYoloIouThreshold;
    int detectYoloConfirmMaxCandidates;
    int detectYoloConfirmNmsRadius;
    bool detectYoloFallbackTraditionalOnEmpty;
    double detectYoloCenterWeightRadius;
    int detectYoloSupplementInterval;
    int detectYoloSupplementMaxWindows;
    double detectYoloSkyCoverageThreshold;

    double detectBackgroundUpdateAlpha;
    int detectBackgroundUpdateInterval;
    int detectBackgroundProtectRadius;

    bool detectAutoSwitchRoi;
    int detectAutoSwitchCooldownMs;
    int detectRadarHoldMs;
    int detectMaxRadarTargets;
};

#endif // APPCONFIG_H
