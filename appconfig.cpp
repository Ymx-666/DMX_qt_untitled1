#include "appconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
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


static QStringList toStringListValue(const QJsonObject &obj, const QString &key, const QStringList &fallback)
{
    if (!obj.contains(key)) return fallback;
    const QJsonValue v = obj.value(key);
    QStringList out;
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            if (arr.at(i).isString()) {
                const QString item = arr.at(i).toString().trimmed();
                if (!item.isEmpty()) out << item;
            }
        }
    } else if (v.isString()) {
        const QStringList parts = v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString item = part.trimmed();
            if (!item.isEmpty()) out << item;
        }
    }
    return out.isEmpty() ? fallback : out;
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

static double toDoubleValue(const QJsonObject &obj, const QString &key, double fallback, double minValue, double maxValue)
{
    if (!obj.contains(key)) return fallback;
    const QJsonValue v = obj.value(key);
    double out = fallback;
    if (v.isDouble()) out = v.toDouble();
    else if (v.isString()) {
        bool ok = false;
        const double parsed = v.toString().toDouble(&ok);
        if (ok) out = parsed;
    }
    if (out < minValue) out = minValue;
    if (out > maxValue) out = maxValue;
    return out;
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
    cfg->rawLogRoot = joinPath(cfg->dataRoot, QStringLiteral("raw_log"));
    cfg->detectSaveRoot = joinPath(cfg->dataRoot, QStringLiteral("candidates"));
    cfg->detectSkyMaskSaveRoot = joinPath(cfg->dataRoot, QStringLiteral("sky_masks"));
}

static QString normalizeDetectStream(QString stream)
{
    stream = stream.trimmed().toUpper();
    if (stream == QStringLiteral("RGB")) return stream;
    if (stream == QStringLiteral("BOTH")) return stream;
    return QStringLiteral("BW");
}

static QString normalizeYoloMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QStringLiteral("confirm")) return mode;
    if (mode == QStringLiteral("sky")) return mode;
    return QStringLiteral("hybrid");
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
        if (paths.contains(QStringLiteral("rawLogRoot"))) {
            const QString v = toStringValue(paths, QStringLiteral("rawLogRoot"), QString()).trimmed();
            if (!v.isEmpty()) cfg->rawLogRoot = v;
            else if (dataRootChanged) cfg->rawLogRoot = joinPath(cfg->dataRoot, QStringLiteral("raw_log"));
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

    if (root.contains(QStringLiteral("camera")) && root.value(QStringLiteral("camera")).isObject()) {
        const QJsonObject camera = root.value(QStringLiteral("camera")).toObject();
        cfg->cameraVerticalFovDeg = toDoubleValue(camera, QStringLiteral("verticalFovDeg"), cfg->cameraVerticalFovDeg, 1.0, 180.0);
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
        cfg->pathPort = (quint16)toIntValue(device, QStringLiteral("pathPort"), cfg->pathPort, 1, 65535);
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

    if (root.contains(QStringLiteral("detect")) && root.value(QStringLiteral("detect")).isObject()) {
        const QJsonObject detect = root.value(QStringLiteral("detect")).toObject();
        cfg->detectEnabled = toBoolValue(detect, QStringLiteral("enabled"), cfg->detectEnabled);
        cfg->detectStream = normalizeDetectStream(toStringValue(detect, QStringLiteral("stream"), cfg->detectStream));
        cfg->detectSaveRoot = toStringValue(detect, QStringLiteral("saveRoot"), cfg->detectSaveRoot).trimmed();
        cfg->detectSkyMaskSaveRoot = toStringValue(
            detect, QStringLiteral("skyMaskSaveRoot"), cfg->detectSkyMaskSaveRoot).trimmed();
        cfg->detectCropSize = toIntValue(detect, QStringLiteral("cropSize"), cfg->detectCropSize, 32, 4096);
        cfg->detectBackgroundFrames = toIntValue(detect, QStringLiteral("backgroundFrames"), cfg->detectBackgroundFrames, 1, 30);
        cfg->detectSkyMargin = toIntValue(detect, QStringLiteral("skyMargin"), cfg->detectSkyMargin, 0, 1024);
        cfg->detectSkyShrinkPixels = toIntValue(detect, QStringLiteral("skyShrinkPixels"), cfg->detectSkyShrinkPixels, 0, 256);
        cfg->detectMaxCandidatesPerFrame = toIntValue(detect, QStringLiteral("maxCandidatesPerFrame"), cfg->detectMaxCandidatesPerFrame, 1, 100);
        cfg->detectMinArea = toIntValue(detect, QStringLiteral("minArea"), cfg->detectMinArea, 1, 1000000);
        cfg->detectMaxArea = toIntValue(detect, QStringLiteral("maxArea"), cfg->detectMaxArea, cfg->detectMinArea, 10000000);
        cfg->detectMedianKernel = toIntValue(detect, QStringLiteral("medianKernel"), cfg->detectMedianKernel, 1, 31);
        cfg->detectTophatKernel = toIntValue(detect, QStringLiteral("tophatKernel"), cfg->detectTophatKernel, 3, 255);
        cfg->detectThresholdK = toDoubleValue(detect, QStringLiteral("thresholdK"), cfg->detectThresholdK, 0.1, 20.0);
        cfg->detectMinContrast = toIntValue(detect, QStringLiteral("minContrast"), cfg->detectMinContrast, 0, 255);
        cfg->detectNmsRadius = toIntValue(detect, QStringLiteral("nmsRadius"), cfg->detectNmsRadius, 0, 4096);
        cfg->detectFeatureLimit = toIntValue(detect, QStringLiteral("featureLimit"), cfg->detectFeatureLimit, 1, 2000);
        cfg->detectFeatureNmsRadius = toIntValue(detect, QStringLiteral("featureNmsRadius"), cfg->detectFeatureNmsRadius, 0, 4096);
        cfg->detectJpegQuality = toIntValue(detect, QStringLiteral("jpegQuality"), cfg->detectJpegQuality, 1, 100);
        cfg->detectReferenceTemplatePath = toStringValue(detect, QStringLiteral("referenceTemplatePath"), cfg->detectReferenceTemplatePath).trimmed();
        cfg->detectBackgroundUpdateAlpha = toDoubleValue(detect, QStringLiteral("backgroundUpdateAlpha"), cfg->detectBackgroundUpdateAlpha, 0.0, 0.20);
        cfg->detectBackgroundUpdateInterval = toIntValue(detect, QStringLiteral("backgroundUpdateInterval"), cfg->detectBackgroundUpdateInterval, 1, 1000);
        cfg->detectBackgroundProtectRadius = toIntValue(detect, QStringLiteral("backgroundProtectRadius"), cfg->detectBackgroundProtectRadius, 0, 4096);
    }



    if (root.contains(QStringLiteral("detectYolo")) && root.value(QStringLiteral("detectYolo")).isObject()) {
        const QJsonObject yolo = root.value(QStringLiteral("detectYolo")).toObject();
        cfg->detectYoloEnabled = toBoolValue(yolo, QStringLiteral("enabled"), cfg->detectYoloEnabled);
        cfg->detectYoloModelPath = toStringValue(yolo, QStringLiteral("modelPath"), cfg->detectYoloModelPath).trimmed();
        cfg->detectYoloMode = normalizeYoloMode(toStringValue(yolo, QStringLiteral("mode"), cfg->detectYoloMode));
        cfg->detectYoloClassNames = toStringListValue(yolo, QStringLiteral("classNames"), cfg->detectYoloClassNames);
        cfg->detectYoloInputSize = toIntValue(yolo, QStringLiteral("inputSize"), cfg->detectYoloInputSize, 32, 4096);
        cfg->detectYoloConfThreshold = toDoubleValue(yolo, QStringLiteral("confThreshold"), cfg->detectYoloConfThreshold, 0.000001, 0.99);
        cfg->detectYoloConfirmThreshold = toDoubleValue(yolo, QStringLiteral("confirmThreshold"), cfg->detectYoloConfThreshold, 0.000001, 0.99);
        cfg->detectYoloSupplementThreshold = toDoubleValue(yolo, QStringLiteral("supplementThreshold"), cfg->detectYoloSupplementThreshold, 0.000001, 0.99);
        cfg->detectYoloIouThreshold = toDoubleValue(yolo, QStringLiteral("iouThreshold"), cfg->detectYoloIouThreshold, 0.01, 0.99);
        cfg->detectYoloConfirmMaxCandidates = toIntValue(yolo, QStringLiteral("confirmMaxCandidates"), cfg->detectYoloConfirmMaxCandidates, 1, 1000);
        cfg->detectYoloConfirmNmsRadius = toIntValue(yolo, QStringLiteral("confirmNmsRadius"), cfg->detectYoloConfirmNmsRadius, 0, 4096);
        cfg->detectYoloFallbackTraditionalOnEmpty = toBoolValue(yolo, QStringLiteral("fallbackTraditionalOnEmpty"), cfg->detectYoloFallbackTraditionalOnEmpty);
        cfg->detectYoloCenterWeightRadius = toDoubleValue(yolo, QStringLiteral("centerWeightRadius"), cfg->detectYoloCenterWeightRadius, 1.0, 4096.0);
        cfg->detectYoloSupplementInterval = toIntValue(yolo, QStringLiteral("supplementInterval"), cfg->detectYoloSupplementInterval, 1, 1000);
        cfg->detectYoloSupplementMaxWindows = toIntValue(yolo, QStringLiteral("supplementMaxWindows"), cfg->detectYoloSupplementMaxWindows, 0, 1000);
        cfg->detectYoloSkyCoverageThreshold = toDoubleValue(yolo, QStringLiteral("skyCoverageThreshold"), cfg->detectYoloSkyCoverageThreshold, 0.05, 1.0);
    }

    if (root.contains(QStringLiteral("detectUi")) && root.value(QStringLiteral("detectUi")).isObject()) {
        const QJsonObject ui = root.value(QStringLiteral("detectUi")).toObject();
        cfg->detectAutoSwitchRoi = toBoolValue(ui, QStringLiteral("autoSwitchRoi"), cfg->detectAutoSwitchRoi);
        cfg->detectAutoSwitchCooldownMs = toIntValue(ui, QStringLiteral("autoSwitchCooldownMs"), cfg->detectAutoSwitchCooldownMs, 0, 600000);
        cfg->detectRadarHoldMs = toIntValue(ui, QStringLiteral("radarHoldMs"), cfg->detectRadarHoldMs, 100, 600000);
        cfg->detectMaxRadarTargets = toIntValue(ui, QStringLiteral("maxRadarTargets"), cfg->detectMaxRadarTargets, 1, 1000);
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
    s = envString("DMX_RAW_LOG_ROOT");
    if (!s.isEmpty()) cfg->rawLogRoot = s;
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
    s = envString("DMX_CAMERA_VERTICAL_FOV_DEG");
    if (!s.isEmpty()) {
        bool ok = false;
        const double fov = s.toDouble(&ok);
        if (ok) cfg->cameraVerticalFovDeg = qBound(1.0, fov, 180.0);
    }
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
    if (envInt("DMX_PATH_PORT", &n)) cfg->pathPort = (quint16)qBound(1, n, 65535);
    if (envBoolCompat("DMX_REPLAY_MODE", &b)) cfg->replayMode = b;

    s = envString("DMX_TURNTABLE_SERIAL_PORT");
    if (!s.isEmpty()) cfg->turntableSerialPort = s;
    if (envInt("DMX_TURNTABLE_BAUD_RATE", &n)) cfg->turntableBaudRate = qBound(1200, n, 921600);
    s = envString("DMX_TURNTABLE_DIRECTION");
    if (!s.isEmpty()) cfg->turntableDirection = normalizeDirection(s);
    if (envInt("DMX_TURNTABLE_SPEED", &n)) cfg->turntableSpeed = qBound(1, n, 255);
    if (envBoolCompat("DMX_TURNTABLE_ORTHO", &b)) cfg->turntableOrthoEnabled = b;
    if (envInt("DMX_TURNTABLE_ORTHO_LENGTH", &n)) cfg->turntableOrthoLength = qBound(1, n, 65535);
    if (envBoolCompat("DMX_TURNTABLE_FEEDBACK", &b)) cfg->turntableFeedbackEnabled = b;

    if (envBoolCompat("DMX_DETECT_ENABLED", &b)) cfg->detectEnabled = b;
    s = envString("DMX_DETECT_STREAM");
    if (!s.isEmpty()) cfg->detectStream = normalizeDetectStream(s);
    s = envString("DMX_DETECT_SAVE_ROOT");
    if (!s.isEmpty()) cfg->detectSaveRoot = s;
    s = envString("DMX_SKY_MASK_SAVE_ROOT");
    if (!s.isEmpty()) cfg->detectSkyMaskSaveRoot = s;
    if (envInt("DMX_DETECT_CROP_SIZE", &n)) cfg->detectCropSize = qBound(32, n, 4096);
    if (envInt("DMX_DETECT_BACKGROUND_FRAMES", &n)) cfg->detectBackgroundFrames = qBound(1, n, 30);
    if (envInt("DMX_DETECT_SKY_MARGIN", &n)) cfg->detectSkyMargin = qBound(0, n, 1024);
    if (envInt("DMX_DETECT_SKY_SHRINK_PIXELS", &n)) cfg->detectSkyShrinkPixels = qBound(0, n, 256);
    if (envInt("DMX_DETECT_MAX_CANDIDATES", &n)) cfg->detectMaxCandidatesPerFrame = qBound(1, n, 100);
    if (envInt("DMX_DETECT_FEATURE_LIMIT", &n)) cfg->detectFeatureLimit = qBound(1, n, 2000);
    if (envInt("DMX_DETECT_FEATURE_NMS_RADIUS", &n)) cfg->detectFeatureNmsRadius = qBound(0, n, 4096);
    s = envString("DMX_DETECT_REFERENCE_TEMPLATE");
    if (!s.isEmpty()) cfg->detectReferenceTemplatePath = s;

    if (envBoolCompat("DMX_YOLO_ENABLED", &b)) cfg->detectYoloEnabled = b;
    s = envString("DMX_YOLO_MODEL");
    if (!s.isEmpty()) cfg->detectYoloModelPath = s;
    s = envString("DMX_YOLO_MODE");
    if (!s.isEmpty()) cfg->detectYoloMode = normalizeYoloMode(s);
    if (envInt("DMX_YOLO_INPUT_SIZE", &n)) cfg->detectYoloInputSize = qBound(32, n, 4096);
    if (envInt("DMX_YOLO_CONFIRM_MAX_CANDIDATES", &n)) cfg->detectYoloConfirmMaxCandidates = qBound(1, n, 1000);
    if (envInt("DMX_YOLO_CONFIRM_NMS_RADIUS", &n)) cfg->detectYoloConfirmNmsRadius = qBound(0, n, 4096);
    if (envInt("DMX_YOLO_SUPPLEMENT_INTERVAL", &n)) cfg->detectYoloSupplementInterval = qBound(1, n, 1000);
    if (envInt("DMX_YOLO_SUPPLEMENT_MAX_WINDOWS", &n)) cfg->detectYoloSupplementMaxWindows = qBound(0, n, 1000);
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
      cameraVerticalFovDeg(26.88),
      thumbWidth(8192),
      thumbHeight(240),
      previewWidth(8192),
      previewJpegQuality(95),
      saveRawTiles(false),
      recordRollMinutes(10),
      deviceIp(QStringLiteral("192.168.4.1")),
      cmdPortSend(5001),
      cmdPortReply(5002),
      pathPort(8001),
      replayMode(false),
      turntableBaudRate(9600),
      turntableDirection(QStringLiteral("left")),
      turntableSpeed(43),
      turntableOrthoEnabled(false),
      turntableOrthoLength(4096),
      turntableFeedbackEnabled(true),
      detectEnabled(false),
      detectStream(QStringLiteral("BW")),
      detectCropSize(256),
      detectBackgroundFrames(3),
      detectSkyMargin(16),
      detectSkyShrinkPixels(16),
      detectMaxCandidatesPerFrame(3),
      detectMinArea(4),
      detectMaxArea(400),
      detectMedianKernel(3),
      detectTophatKernel(15),
      detectThresholdK(3.5),
      detectMinContrast(25),
      detectNmsRadius(96),
      detectFeatureLimit(120),
      detectFeatureNmsRadius(56),
      detectJpegQuality(95),
      detectReferenceTemplatePath(QString()),
      detectYoloEnabled(false),
      detectYoloModelPath(QStringLiteral("/home/sht/data/低小慢数据/yolo26s/result/jiao/pt/weights2/best.onnx")),
      detectYoloMode(QStringLiteral("confirm")),
      detectYoloClassNames(QStringList() << QStringLiteral("drone") << QStringLiteral("bird")),
      detectYoloInputSize(640),
      detectYoloConfThreshold(0.03),
      detectYoloConfirmThreshold(0.03),
      detectYoloSupplementThreshold(0.08),
      detectYoloIouThreshold(0.45),
      detectYoloConfirmMaxCandidates(8),
      detectYoloConfirmNmsRadius(256),
      detectYoloFallbackTraditionalOnEmpty(false),
      detectYoloCenterWeightRadius(256.0),
      detectYoloSupplementInterval(8),
      detectYoloSupplementMaxWindows(6),
      detectYoloSkyCoverageThreshold(0.70),
      detectBackgroundUpdateAlpha(0.02),
      detectBackgroundUpdateInterval(4),
      detectBackgroundProtectRadius(128),
      detectAutoSwitchRoi(true),
      detectAutoSwitchCooldownMs(1000),
      detectRadarHoldMs(5000),
      detectMaxRadarTargets(20)
{
#ifdef Q_OS_WIN
    dataRoot = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../data")));
    saveRoot = QDir(dataRoot).filePath(QStringLiteral("SAVES"));
    recordRoot = QStringLiteral("D:/DMX_data");
    logRoot = QDir(dataRoot).filePath(QStringLiteral("logs"));
    rawLogRoot = QDir(dataRoot).filePath(QStringLiteral("raw_log"));
    detectSaveRoot = QDir(dataRoot).filePath(QStringLiteral("candidates"));
    detectSkyMaskSaveRoot = QDir(dataRoot).filePath(QStringLiteral("sky_masks"));
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
