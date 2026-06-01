#include "appconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <cstdlib>

static QString toStringValue(const QJsonObject &obj, const QString &key, const QString &fallback)
{
    if (!obj.contains(key)) return fallback;
    const QJsonValue v = obj.value(key);
    if (v.isString()) return v.toString();
    return fallback;
}

static int toIntValue(const QJsonObject &obj, const QString &key, int fallback, int minValue, int maxValue)
{
    if (!obj.contains(key)) return fallback;
    const QJsonValue v = obj.value(key);
    int out = fallback;
    if (v.isDouble()) out = (int)v.toDouble();
    else if (v.isString()) {
        bool ok = false;
        const int parsed = v.toString().toInt(&ok);
        if (ok) out = parsed;
    }
    return qBound(minValue, out, maxValue);
}

static bool toBoolValue(const QJsonObject &obj, const QString &key, bool fallback)
{
    if (!obj.contains(key)) return fallback;
    const QJsonValue v = obj.value(key);
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return ((int)v.toDouble()) != 0;
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QStringLiteral("1") || s == QStringLiteral("true") || s == QStringLiteral("yes") || s == QStringLiteral("on")) return true;
        if (s == QStringLiteral("0") || s == QStringLiteral("false") || s == QStringLiteral("no") || s == QStringLiteral("off")) return false;
    }
    return fallback;
}

static QString joinPath(const QString &root, const QString &leaf)
{
    if (root.isEmpty()) return QString();
    return QDir(root).filePath(leaf);
}

static QString normalizeDirection(QString direction)
{
    direction = direction.trimmed().toLower();
    if (direction == QStringLiteral("left")) return direction;
    return QStringLiteral("right");
}

static bool envBytes(const char *name, QByteArray *out)
{
    const char *p = std::getenv(name);
    if (!p) return false;
    if (out) *out = QByteArray(p);
    return true;
}

static QString envString(const char *name)
{
    QByteArray v;
    if (!envBytes(name, &v)) return QString();
    return QString::fromLocal8Bit(v).trimmed();
}

static bool envInt(const char *name, int *out)
{
    QByteArray v;
    if (!envBytes(name, &v)) return false;
    bool ok = false;
    const int parsed = QString::fromLatin1(v).trimmed().toInt(&ok);
    if (ok && out) *out = parsed;
    return ok;
}

static bool envBoolCompat(const char *name, bool *out)
{
    QByteArray v;
    if (!envBytes(name, &v)) return false;
    const QString s = QString::fromLatin1(v).trimmed().toLower();
    if (s == QStringLiteral("1") || s == QStringLiteral("true") || s == QStringLiteral("yes") || s == QStringLiteral("on")) {
        if (out) *out = true;
        return true;
    }
    if (s == QStringLiteral("0") || s == QStringLiteral("false") || s == QStringLiteral("no") || s == QStringLiteral("off")) {
        if (out) *out = false;
        return true;
    }
    return false;
}

static QString firstExistingConfigPath()
{
    const QString envPath = envString("DMX_CONFIG");
    if (!envPath.isEmpty()) return envPath;

    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        candidates << QDir(appDir).filePath(QStringLiteral("dmx_config.json"));
        candidates << QDir(appDir).filePath(QStringLiteral("../dmx_config.json"));
    }
    candidates << QDir::current().filePath(QStringLiteral("dmx_config.json"));

    for (int i = 0; i < candidates.size(); ++i) {
        if (QFileInfo(candidates.at(i)).isFile()) return QDir::cleanPath(candidates.at(i));
    }
    return QString();
}

static void deriveRootsFromDataRoot(AppConfig *cfg)
{
    if (!cfg) return;
    cfg->saveRoot = joinPath(cfg->dataRoot, QStringLiteral("saves"));
    cfg->recordRoot = joinPath(cfg->dataRoot, QStringLiteral("recordings"));
    cfg->logRoot = joinPath(cfg->dataRoot, QStringLiteral("logs"));
}

static void applyJson(AppConfig *cfg, const QJsonObject &root)
{
    if (!cfg) return;

    if (root.contains(QStringLiteral("paths")) && root.value(QStringLiteral("paths")).isObject()) {
        const QJsonObject paths = root.value(QStringLiteral("paths")).toObject();
        const bool hasDataRoot = paths.contains(QStringLiteral("dataRoot"));
        bool dataRootChanged = false;
        if (hasDataRoot) {
            const QString dataRoot = toStringValue(paths, QStringLiteral("dataRoot"), cfg->dataRoot).trimmed();
            if (!dataRoot.isEmpty()) {
                cfg->dataRoot = dataRoot;
                deriveRootsFromDataRoot(cfg);
                dataRootChanged = true;
            }
        }
        if (paths.contains(QStringLiteral("saveRoot"))) {
            const QString v = toStringValue(paths, QStringLiteral("saveRoot"), QString()).trimmed();
            if (!v.isEmpty()) cfg->saveRoot = v;
            else if (dataRootChanged) cfg->saveRoot = joinPath(cfg->dataRoot, QStringLiteral("saves"));
        }
        if (paths.contains(QStringLiteral("recordRoot"))) {
            const QString v = toStringValue(paths, QStringLiteral("recordRoot"), QString()).trimmed();
            if (!v.isEmpty()) cfg->recordRoot = v;
            else if (dataRootChanged) cfg->recordRoot = joinPath(cfg->dataRoot, QStringLiteral("recordings"));
        }
        if (paths.contains(QStringLiteral("logRoot"))) {
            const QString v = toStringValue(paths, QStringLiteral("logRoot"), QString()).trimmed();
            if (!v.isEmpty()) cfg->logRoot = v;
            else if (dataRootChanged) cfg->logRoot = joinPath(cfg->dataRoot, QStringLiteral("logs"));
        }
        cfg->shareMount = toStringValue(paths, QStringLiteral("shareMount"), cfg->shareMount).trimmed();
    }

    if (root.contains(QStringLiteral("capture")) && root.value(QStringLiteral("capture")).isObject()) {
        const QJsonObject capture = root.value(QStringLiteral("capture")).toObject();
        cfg->angleLookup = toBoolValue(capture, QStringLiteral("angleLookup"), cfg->angleLookup);
        cfg->captureLatencyMs = toIntValue(capture, QStringLiteral("latencyMs"), (int)cfg->captureLatencyMs, -60000, 60000);
        cfg->captureLatencyRgbMs = toIntValue(capture, QStringLiteral("latencyRgbMs"), (int)cfg->captureLatencyRgbMs, -60000, 60000);
        cfg->captureLatencyBwMs = toIntValue(capture, QStringLiteral("latencyBwMs"), (int)cfg->captureLatencyBwMs, -60000, 60000);
        cfg->uiDropStale = toBoolValue(capture, QStringLiteral("uiDropStale"), cfg->uiDropStale);
        cfg->uiQueueCap = toIntValue(capture, QStringLiteral("uiQueueCap"), cfg->uiQueueCap, 1, 256);
    }

    if (root.contains(QStringLiteral("panorama")) && root.value(QStringLiteral("panorama")).isObject()) {
        const QJsonObject pano = root.value(QStringLiteral("panorama")).toObject();
        cfg->fullWidth = toIntValue(pano, QStringLiteral("fullWidth"), cfg->fullWidth, 1024, 262144);
        cfg->fullHeight = toIntValue(pano, QStringLiteral("fullHeight"), cfg->fullHeight, 256, 32768);
        cfg->thumbWidth = toIntValue(pano, QStringLiteral("thumbWidth"), cfg->thumbWidth, 1024, 65536);
        cfg->thumbHeight = toIntValue(pano, QStringLiteral("thumbHeight"), cfg->thumbHeight, 64, 4096);
        cfg->previewWidth = toIntValue(pano, QStringLiteral("previewWidth"), cfg->previewWidth, 512, 65535);
        cfg->previewJpegQuality = toIntValue(pano, QStringLiteral("previewJpegQuality"), cfg->previewJpegQuality, 1, 100);
        cfg->saveRawTiles = toBoolValue(pano, QStringLiteral("saveRawTiles"), cfg->saveRawTiles);
    }

    if (root.contains(QStringLiteral("recording")) && root.value(QStringLiteral("recording")).isObject()) {
        const QJsonObject recording = root.value(QStringLiteral("recording")).toObject();
        cfg->recordRollMinutes = toIntValue(recording, QStringLiteral("rollMinutes"), cfg->recordRollMinutes, 1, 1440);
    }

    if (root.contains(QStringLiteral("device")) && root.value(QStringLiteral("device")).isObject()) {
        const QJsonObject device = root.value(QStringLiteral("device")).toObject();
        cfg->deviceIp = toStringValue(device, QStringLiteral("ip"), cfg->deviceIp).trimmed();
        cfg->cmdPortSend = (quint16)toIntValue(device, QStringLiteral("cmdPortSend"), cfg->cmdPortSend, 1, 65535);
        cfg->cmdPortReply = (quint16)toIntValue(device, QStringLiteral("cmdPortReply"), cfg->cmdPortReply, 1, 65535);
    }

    if (root.contains(QStringLiteral("turntable")) && root.value(QStringLiteral("turntable")).isObject()) {
        const QJsonObject turntable = root.value(QStringLiteral("turntable")).toObject();
        cfg->turntableSerialPort = toStringValue(turntable, QStringLiteral("serialPort"), cfg->turntableSerialPort).trimmed();
        cfg->turntableBaudRate = toIntValue(turntable, QStringLiteral("baudRate"), cfg->turntableBaudRate, 1200, 921600);
        cfg->turntableDirection = normalizeDirection(toStringValue(turntable, QStringLiteral("direction"), cfg->turntableDirection));
        cfg->turntableSpeed = toIntValue(turntable, QStringLiteral("speed"), cfg->turntableSpeed, 1, 255);
        cfg->turntableOrthoEnabled = toBoolValue(turntable, QStringLiteral("orthoEnabled"), cfg->turntableOrthoEnabled);
        cfg->turntableOrthoLength = toIntValue(turntable, QStringLiteral("orthoLength"), cfg->turntableOrthoLength, 1, 65535);
        cfg->turntableFeedbackEnabled = toBoolValue(turntable, QStringLiteral("feedbackEnabled"), cfg->turntableFeedbackEnabled);
    }
}

static void applyEnvironment(AppConfig *cfg)
{
    if (!cfg) return;

    QString s = envString("DMX_DATA_ROOT");
    if (!s.isEmpty()) {
        cfg->dataRoot = s;
        deriveRootsFromDataRoot(cfg);
    }
    s = envString("DMX_SAVE_ROOT");
    if (!s.isEmpty()) cfg->saveRoot = s;
    s = envString("DMX_REC_ROOT");
    if (!s.isEmpty()) cfg->recordRoot = s;
    s = envString("DMX_LOG_ROOT");
    if (!s.isEmpty()) cfg->logRoot = s;
    s = envString("DMX_SHARE_MOUNT");
    if (!s.isEmpty()) cfg->shareMount = s;

    bool b = false;
    if (envBoolCompat("DMX_ANGLE_LOOKUP", &b)) cfg->angleLookup = b;

    int n = 0;
    if (envInt("DMX_CAPTURE_LATENCY_MS", &n)) {
        cfg->captureLatencyMs = n;
        cfg->captureLatencyRgbMs = n;
        cfg->captureLatencyBwMs = n;
    }
    if (envInt("DMX_CAPTURE_LATENCY_MS_RGB", &n)) cfg->captureLatencyRgbMs = n;
    if (envInt("DMX_CAPTURE_LATENCY_MS_BW", &n)) cfg->captureLatencyBwMs = n;

    QByteArray raw;
    if (envBytes("DMX_UI_NODROP", &raw)) {
        cfg->uiDropStale = (QString::fromLatin1(raw).trimmed() != QStringLiteral("1"));
    }
    if (envInt("DMX_UI_QUEUE_CAP", &n)) cfg->uiQueueCap = qBound(1, n, 256);

    if (envInt("DMX_PANO_FULL_WIDTH", &n)) cfg->fullWidth = qBound(1024, n, 262144);
    if (envInt("DMX_PANO_FULL_HEIGHT", &n)) cfg->fullHeight = qBound(256, n, 32768);
    if (envInt("DMX_PANO_THUMB_WIDTH", &n)) cfg->thumbWidth = qBound(1024, n, 65536);
    if (envInt("DMX_PANO_THUMB_HEIGHT", &n)) cfg->thumbHeight = qBound(64, n, 4096);
    if (envInt("DMX_PANO_PREVIEW_WIDTH", &n)) cfg->previewWidth = qBound(512, n, 65535);
    if (envInt("DMX_PANO_PREVIEW_JPEG_QUALITY", &n)) cfg->previewJpegQuality = qBound(1, n, 100);
    if (envBoolCompat("DMX_PANO_SAVE_RAW", &b)) cfg->saveRawTiles = b;
    if (envBoolCompat("PANO_SAVE_RAW", &b)) cfg->saveRawTiles = b;

    if (envInt("DMX_REC_ROLL_MINUTES", &n)) cfg->recordRollMinutes = qBound(1, n, 1440);

    s = envString("DMX_DEVICE_IP");
    if (!s.isEmpty()) cfg->deviceIp = s;
    if (envInt("DMX_CMD_PORT_SEND", &n)) cfg->cmdPortSend = (quint16)qBound(1, n, 65535);
    if (envInt("DMX_CMD_PORT_REPLY", &n)) cfg->cmdPortReply = (quint16)qBound(1, n, 65535);

    s = envString("DMX_TURNTABLE_SERIAL_PORT");
    if (!s.isEmpty()) cfg->turntableSerialPort = s;
    if (envInt("DMX_TURNTABLE_BAUD_RATE", &n)) cfg->turntableBaudRate = qBound(1200, n, 921600);
    s = envString("DMX_TURNTABLE_DIRECTION");
    if (!s.isEmpty()) cfg->turntableDirection = normalizeDirection(s);
    if (envInt("DMX_TURNTABLE_SPEED", &n)) cfg->turntableSpeed = qBound(1, n, 255);
    if (envBoolCompat("DMX_TURNTABLE_ORTHO", &b)) cfg->turntableOrthoEnabled = b;
    if (envInt("DMX_TURNTABLE_ORTHO_LENGTH", &n)) cfg->turntableOrthoLength = qBound(1, n, 65535);
    if (envBoolCompat("DMX_TURNTABLE_FEEDBACK", &b)) cfg->turntableFeedbackEnabled = b;
}

AppConfig::AppConfig()
    : loadedFromFile(false),
      angleLookup(true),
      captureLatencyMs(0),
      captureLatencyRgbMs(300),
      captureLatencyBwMs(200),
      uiDropStale(true),
      uiQueueCap(2),
      fullWidth(65536),
      fullHeight(4096),
      thumbWidth(8192),
      thumbHeight(240),
      previewWidth(8192),
      previewJpegQuality(95),
      saveRawTiles(false),
      recordRollMinutes(10),
      deviceIp(QStringLiteral("192.168.4.1")),
      cmdPortSend(5001),
      cmdPortReply(5002),
      turntableBaudRate(9600),
      turntableDirection(QStringLiteral("right")),
      turntableSpeed(85),
      turntableOrthoEnabled(false),
      turntableOrthoLength(4096),
      turntableFeedbackEnabled(true)
{
#ifdef Q_OS_WIN
    dataRoot = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../data")));
    saveRoot = QDir(dataRoot).filePath(QStringLiteral("SAVES"));
    recordRoot = QStringLiteral("D:/DMX_data");
    logRoot = QDir(dataRoot).filePath(QStringLiteral("logs"));
    shareMount = QStringLiteral("/mnt/dmx_share");
#else
    dataRoot = QStringLiteral("/media/sht/6C3CCFC13CCF8494/data");
    deriveRootsFromDataRoot(this);
    shareMount = QStringLiteral("/mnt/dmx_share");
#endif
}

const AppConfig &AppConfig::instance()
{
    static AppConfig cfg = AppConfig::loadDefault(true);
    return cfg;
}

AppConfig AppConfig::loadDefault(bool applyEnvironmentFlag)
{
    const QString path = firstExistingConfigPath();
    return loadFile(path, applyEnvironmentFlag);
}

AppConfig AppConfig::loadFile(const QString &path, bool applyEnvironmentFlag)
{
    AppConfig cfg;
    cfg.loadedPath = path;

    if (!path.trimmed().isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            f.close();
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                applyJson(&cfg, doc.object());
                cfg.loadedFromFile = true;
            } else {
                cfg.loadError = QStringLiteral("JSON parse failed: %1").arg(parseError.errorString());
            }
        } else {
            cfg.loadError = QStringLiteral("Open failed: %1").arg(path);
        }
    }

    if (applyEnvironmentFlag) {
        applyEnvironment(&cfg);
    }
    cfg.turntableDirection = normalizeDirection(cfg.turntableDirection);
    return cfg;
}
