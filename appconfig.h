#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
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
    QString shareMount;

    bool angleLookup;
    qint64 captureLatencyMs;
    qint64 captureLatencyRgbMs;
    qint64 captureLatencyBwMs;
    bool uiDropStale;
    int uiQueueCap;

    int fullWidth;
    int fullHeight;
    int thumbWidth;
    int thumbHeight;
    int previewWidth;
    int previewJpegQuality;
    bool saveRawTiles;

    int recordRollMinutes;

    QString deviceIp;
    quint16 cmdPortSend;
    quint16 cmdPortReply;

    QString turntableSerialPort;
    int turntableBaudRate;
    QString turntableDirection;
    int turntableSpeed;
    bool turntableOrthoEnabled;
    int turntableOrthoLength;
    bool turntableFeedbackEnabled;
};

#endif // APPCONFIG_H
