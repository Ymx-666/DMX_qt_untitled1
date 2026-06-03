#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "appconfig.h"

class AppConfigTests : public QObject
{
    Q_OBJECT
private slots:
    void defaultWindowsPathsDoNotUseOldProjectName()
    {
#ifdef Q_OS_WIN
        const AppConfig cfg = AppConfig::loadFile(QString(), false);
        QVERIFY(!cfg.dataRoot.contains(QStringLiteral("untitled1")));
        QVERIFY(!cfg.saveRoot.contains(QStringLiteral("untitled1")));
        QVERIFY(!cfg.logRoot.contains(QStringLiteral("untitled1")));
        QVERIFY(cfg.dataRoot.contains(QStringLiteral("DMX")));
        QVERIFY(cfg.saveRoot.contains(QStringLiteral("DMX")));
        QVERIFY(cfg.logRoot.contains(QStringLiteral("DMX")));
#endif
    }

    void jsonValuesOverrideDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/dmx_config.json");

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{"
                "\"paths\":{\"dataRoot\":\"/data/dmx\",\"saveRoot\":\"\",\"recordRoot\":\"/rec\",\"logRoot\":\"\",\"shareMount\":\"/mnt/share\"},"
                "\"capture\":{\"angleLookup\":false,\"latencyMs\":10,\"latencyRgbMs\":300,\"latencyBwMs\":200,\"uiDropStale\":false,\"uiQueueCap\":4},"
                "\"panorama\":{\"fullWidth\":65536,\"fullHeight\":4096,\"thumbWidth\":8192,\"thumbHeight\":240,\"previewWidth\":4096,\"previewJpegQuality\":90},"
                "\"recording\":{\"rollMinutes\":7},"
                "\"device\":{\"ip\":\"192.168.4.9\",\"cmdPortSend\":5011,\"cmdPortReply\":5012},"
                "\"turntable\":{\"serialPort\":\"COM7\",\"baudRate\":115200,\"direction\":\"left\",\"speed\":57,\"orthoEnabled\":true,\"orthoLength\":2048,\"feedbackEnabled\":false}"
                "}");
        f.close();

        const AppConfig cfg = AppConfig::loadFile(path, false);

        QCOMPARE(cfg.dataRoot, QStringLiteral("/data/dmx"));
        QCOMPARE(cfg.saveRoot, QStringLiteral("/data/dmx/saves"));
        QCOMPARE(cfg.recordRoot, QStringLiteral("/rec"));
        QCOMPARE(cfg.logRoot, QStringLiteral("/data/dmx/logs"));
        QCOMPARE(cfg.shareMount, QStringLiteral("/mnt/share"));
        QCOMPARE(cfg.angleLookup, false);
        QCOMPARE((int)cfg.captureLatencyMs, 10);
        QCOMPARE((int)cfg.captureLatencyRgbMs, 300);
        QCOMPARE((int)cfg.captureLatencyBwMs, 200);
        QCOMPARE(cfg.uiDropStale, false);
        QCOMPARE(cfg.uiQueueCap, 4);
        QCOMPARE(cfg.fullWidth, 65536);
        QCOMPARE(cfg.fullHeight, 4096);
        QCOMPARE(cfg.thumbWidth, 8192);
        QCOMPARE(cfg.thumbHeight, 240);
        QCOMPARE(cfg.previewWidth, 4096);
        QCOMPARE(cfg.previewJpegQuality, 90);
        QCOMPARE(cfg.recordRollMinutes, 7);
        QCOMPARE(cfg.deviceIp, QStringLiteral("192.168.4.9"));
        QCOMPARE((int)cfg.cmdPortSend, 5011);
        QCOMPARE((int)cfg.cmdPortReply, 5012);
        QCOMPARE(cfg.turntableSerialPort, QStringLiteral("COM7"));
        QCOMPARE(cfg.turntableBaudRate, 115200);
        QCOMPARE(cfg.turntableDirection, QStringLiteral("left"));
        QCOMPARE(cfg.turntableSpeed, 57);
        QCOMPARE(cfg.turntableOrthoEnabled, true);
        QCOMPARE(cfg.turntableOrthoLength, 2048);
        QCOMPARE(cfg.turntableFeedbackEnabled, false);
    }

    void environmentOverridesJson()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/dmx_config.json");

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{"
                "\"paths\":{\"saveRoot\":\"/json/save\"},"
                "\"capture\":{\"latencyRgbMs\":111,\"uiDropStale\":true},"
                "\"turntable\":{\"direction\":\"left\",\"speed\":57}"
                "}");
        f.close();

        qputenv("DMX_SAVE_ROOT", "/env/save");
        qputenv("DMX_CAPTURE_LATENCY_MS_RGB", "345");
        qputenv("DMX_UI_NODROP", "1");
        qputenv("DMX_TURNTABLE_DIRECTION", "right");
        qputenv("DMX_TURNTABLE_SPEED", "85");

        const AppConfig cfg = AppConfig::loadFile(path, true);

        QCOMPARE(cfg.saveRoot, QStringLiteral("/env/save"));
        QCOMPARE((int)cfg.captureLatencyRgbMs, 345);
        QCOMPARE(cfg.uiDropStale, false);
        QCOMPARE(cfg.turntableDirection, QStringLiteral("right"));
        QCOMPARE(cfg.turntableSpeed, 85);

        qunsetenv("DMX_SAVE_ROOT");
        qunsetenv("DMX_CAPTURE_LATENCY_MS_RGB");
        qunsetenv("DMX_UI_NODROP");
        qunsetenv("DMX_TURNTABLE_DIRECTION");
        qunsetenv("DMX_TURNTABLE_SPEED");
    }

    void turntableOrthoCannotBeDisabled()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/dmx_config.json");

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{\"turntable\":{\"orthoEnabled\":false}}");
        f.close();

        qputenv("DMX_TURNTABLE_ORTHO", "0");
        const AppConfig cfg = AppConfig::loadFile(path, true);

        QCOMPARE(cfg.turntableOrthoEnabled, true);
        qunsetenv("DMX_TURNTABLE_ORTHO");
    }
};

QTEST_MAIN(AppConfigTests)
#include "appconfig_tests.moc"
