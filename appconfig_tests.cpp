#include <QtTest/QtTest>
#include <QComboBox>
#include <QFile>
#include <QTemporaryDir>

#include "appconfig.h"
#include "turntablecontroldialog.h"
#include "turntabledriver.h"

class AppConfigTests : public QObject
{
    Q_OBJECT
private slots:
    void turntableDefaultsUseEightSecondGear()
    {
        const AppConfig cfg;
        QCOMPARE(cfg.turntableSpeed, 43);
    }

    void checkedInConfigUsesEightSecondGear()
    {
        const QString path = QFINDTESTDATA("dmx_config.json");
        QVERIFY2(!path.isEmpty(), "dmx_config.json was not found by QtTest");
        const AppConfig cfg = AppConfig::loadFile(path, false);
        QVERIFY2(cfg.loadError.isEmpty(), qPrintable(cfg.loadError));
        QCOMPARE(cfg.turntableSpeed, 43);
    }

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
                "\"camera\":{\"verticalFovDeg\":26.88},"
                "\"recording\":{\"rollMinutes\":7},"
                "\"device\":{\"ip\":\"192.168.4.9\",\"cmdPortSend\":5011,\"cmdPortReply\":5012,\"pathPort\":8011},"
                "\"turntable\":{\"serialPort\":\"COM7\",\"baudRate\":115200,\"direction\":\"left\",\"speed\":57,\"orthoEnabled\":false,\"orthoLength\":2048,\"feedbackEnabled\":false}"
                "}");
        f.close();

        const AppConfig cfg = AppConfig::loadFile(path, false);

        QCOMPARE(cfg.dataRoot, QStringLiteral("/data/dmx"));
        QCOMPARE(cfg.saveRoot, QStringLiteral("/data/dmx/saves"));
        QCOMPARE(cfg.recordRoot, QStringLiteral("/rec"));
        QCOMPARE(cfg.logRoot, QStringLiteral("/data/dmx/logs"));
        QCOMPARE(cfg.detectSkyMaskSaveRoot, QStringLiteral("/data/dmx/sky_masks"));
        QCOMPARE(cfg.shareMount, QStringLiteral("/mnt/share"));
        QCOMPARE(cfg.angleLookup, false);
        QCOMPARE((int)cfg.captureLatencyMs, 10);
        QCOMPARE((int)cfg.captureLatencyRgbMs, 300);
        QCOMPARE((int)cfg.captureLatencyBwMs, 200);
        QCOMPARE(cfg.uiDropStale, false);
        QCOMPARE(cfg.uiQueueCap, 4);
        QCOMPARE(cfg.fullWidth, 65536);
        QCOMPARE(cfg.fullHeight, 4096);
        QVERIFY(qAbs(cfg.cameraVerticalFovDeg - 26.88) < 0.0001);
        QCOMPARE(cfg.thumbWidth, 8192);
        QCOMPARE(cfg.thumbHeight, 240);
        QCOMPARE(cfg.previewWidth, 4096);
        QCOMPARE(cfg.previewJpegQuality, 90);
        QCOMPARE(cfg.recordRollMinutes, 7);
        QCOMPARE(cfg.deviceIp, QStringLiteral("192.168.4.9"));
        QCOMPARE((int)cfg.cmdPortSend, 5011);
        QCOMPARE((int)cfg.cmdPortReply, 5012);
        QCOMPARE((int)cfg.pathPort, 8011);
        QCOMPARE(cfg.replayMode, false);
        QCOMPARE(cfg.turntableSerialPort, QStringLiteral("COM7"));
        QCOMPARE(cfg.turntableBaudRate, 115200);
        QCOMPARE(cfg.turntableDirection, QStringLiteral("left"));
        QCOMPARE(cfg.turntableSpeed, 57);
        QCOMPARE(cfg.turntableOrthoEnabled, false);
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
        qputenv("DMX_SKY_MASK_SAVE_ROOT", "/env/sky_masks");
        qputenv("DMX_CAPTURE_LATENCY_MS_RGB", "345");
        qputenv("DMX_UI_NODROP", "1");
        qputenv("DMX_TURNTABLE_DIRECTION", "right");
        qputenv("DMX_TURNTABLE_SPEED", "85");
        qputenv("DMX_PATH_PORT", "18001");
        qputenv("DMX_REPLAY_MODE", "1");
        qputenv("DMX_CAMERA_VERTICAL_FOV_DEG", "31.5");

        const AppConfig cfg = AppConfig::loadFile(path, true);

        QCOMPARE(cfg.saveRoot, QStringLiteral("/env/save"));
        QCOMPARE(cfg.detectSkyMaskSaveRoot, QStringLiteral("/env/sky_masks"));
        QCOMPARE((int)cfg.captureLatencyRgbMs, 345);
        QCOMPARE(cfg.uiDropStale, false);
        QCOMPARE(cfg.turntableDirection, QStringLiteral("right"));
        QCOMPARE(cfg.turntableSpeed, 85);
        QCOMPARE((int)cfg.pathPort, 18001);
        QCOMPARE(cfg.replayMode, true);
        QVERIFY(qAbs(cfg.cameraVerticalFovDeg - 31.5) < 0.0001);

        qunsetenv("DMX_SAVE_ROOT");
        qunsetenv("DMX_SKY_MASK_SAVE_ROOT");
        qunsetenv("DMX_CAPTURE_LATENCY_MS_RGB");
        qunsetenv("DMX_UI_NODROP");
        qunsetenv("DMX_TURNTABLE_DIRECTION");
        qunsetenv("DMX_TURNTABLE_SPEED");
        qunsetenv("DMX_PATH_PORT");
        qunsetenv("DMX_REPLAY_MODE");
        qunsetenv("DMX_CAMERA_VERTICAL_FOV_DEG");
    }

    void turntableOrthoConfigCanStartDisabled()
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

        QCOMPARE(cfg.turntableOrthoEnabled, false);
        qunsetenv("DMX_TURNTABLE_ORTHO");
    }

    void automaticStartupGearIsIndependentFromManualSelection()
    {
        TurntableDriver driver;
        TurntableControlDialog dialog(&driver);
        TurntableControlDialog::Settings configured;
        configured.serialPort = QStringLiteral("TEST_PORT");
        configured.baudRate = 9600;
        configured.direction = QStringLiteral("left");
        configured.speed = 43;
        configured.orthoEnabled = false;
        configured.orthoLength = 4096;
        configured.feedbackEnabled = true;
        dialog.applySettings(configured);

        QComboBox *speedCombo = dialog.findChild<QComboBox *>(QStringLiteral("turntableSpeedCombo"));
        QVERIFY(speedCombo != nullptr);
        QCOMPARE(speedCombo->currentData().toInt(), 43);
        const int manualSixSecondIndex = speedCombo->findData(57);
        QVERIFY(manualSixSecondIndex >= 0);
        speedCombo->setCurrentIndex(manualSixSecondIndex);

        QCOMPARE(dialog.currentSettings().speed, 57);
        QCOMPARE(dialog.startupSettings().speed, 43);
    }

    void eightSecondTurnCommandUsesSpeedByte43()
    {
        const QByteArray packet = TurntableDriver::buildCommandPacket(0x00, 0x04, 43, 0x00);
        QCOMPARE(packet.size(), 7);
        QCOMPARE(static_cast<unsigned char>(packet.at(0)), static_cast<unsigned char>(0xFF));
        QCOMPARE(static_cast<unsigned char>(packet.at(3)), static_cast<unsigned char>(0x04));
        QCOMPARE(static_cast<unsigned char>(packet.at(4)), static_cast<unsigned char>(43));
        QCOMPARE(static_cast<unsigned char>(packet.at(6)), static_cast<unsigned char>(0x30));
    }
};

QTEST_MAIN(AppConfigTests)
#include "appconfig_tests.moc"
