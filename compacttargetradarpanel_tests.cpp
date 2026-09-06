#include "radar_ui/compacttargetradarpanel.h"
#include "radar_ui/targetinfopanel.h"

#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QtTest>

#ifdef DMX_TEST_BUILD
#error compacttargetradarpanel_tests must exercise the formal DMX build path
#endif

class CompactTargetRadarPanelTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void formalBuildUsesUnifiedThreeColumnLayout();
    void targetSelectionShowsClassElevationAndPreview();
    void targetInfoPanelShowsFovAndElevation();
};

void CompactTargetRadarPanelTests::initTestCase()
{
    qputenv("DMX_CAMERA_VERTICAL_FOV_DEG", QByteArray("26.88"));
}

void CompactTargetRadarPanelTests::formalBuildUsesUnifiedThreeColumnLayout()
{
    CompactTargetRadarPanel panel;
    panel.resize(1320, 320);
    panel.show();
    QTest::qWait(30);

    QWidget *listPane = panel.findChild<QWidget*>(
        QStringLiteral("compactTargetListPane"), Qt::FindDirectChildrenOnly);
    QWidget *radar = panel.findChild<QWidget*>(
        QStringLiteral("compactTargetRadar"), Qt::FindDirectChildrenOnly);
    QWidget *previewPane = panel.findChild<QWidget*>(
        QStringLiteral("compactTargetPreviewPane"), Qt::FindDirectChildrenOnly);
    QVERIFY(listPane);
    QVERIFY(radar);
    QVERIFY(previewPane);
    QVERIFY(listPane->isVisible());
    QVERIFY(radar->isVisible());
    QVERIFY(previewPane->isVisible());

    QVERIFY(!listPane->geometry().intersects(radar->geometry()));
    QVERIFY(!radar->geometry().intersects(previewPane->geometry()));
    QVERIFY(!listPane->geometry().intersects(previewPane->geometry()));
    QVERIFY(panel.rect().contains(listPane->geometry()));
    QVERIFY(panel.rect().contains(radar->geometry()));
    QVERIFY(panel.rect().contains(previewPane->geometry()));
    QVERIFY(radar->width() > listPane->width());
    QVERIFY(radar->width() > previewPane->width());
}

void CompactTargetRadarPanelTests::targetSelectionShowsClassElevationAndPreview()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("target.png"));
    QImage image(320, 180, QImage::Format_RGB32);
    image.fill(QColor(38, 92, 128));
    QVERIFY(image.save(imagePath));

    TargetRecord target;
    target.id = QStringLiteral("T-0001");
    target.time = QDateTime::currentDateTime();
    target.lastTime = target.time;
    target.imagePath = imagePath;
    target.stream = QStringLiteral("RGB+BW");
    target.className = QStringLiteral("drone");
    target.state = QStringLiteral("confirmed");
    target.azimuthDeg = 123.4;
    target.confidence = 0.82;
    target.hasElevationAngle = true;
    target.elevationAngleDeg = 6.25;
    target.hasRoiBox = true;
    target.roiBoxX1 = 80;
    target.roiBoxY1 = 45;
    target.roiBoxX2 = 160;
    target.roiBoxY2 = 105;

    CompactTargetRadarPanel panel;
    panel.resize(1320, 320);
    panel.show();
    panel.setTargets(QVector<TargetRecord>() << target);
    QTest::qWait(50);

    QListWidget *list = panel.findChild<QListWidget*>(QStringLiteral("compactTargetList"));
    QLabel *row = panel.findChild<QLabel*>(QStringLiteral("compactTargetListRow"));
    QLabel *preview = panel.findChild<QLabel*>(QStringLiteral("compactTargetPreview"));
    QLabel *details = panel.findChild<QLabel*>(QStringLiteral("compactTargetPreviewDetails"));
    QVERIFY(list);
    QVERIFY(row);
    QVERIFY(preview);
    QVERIFY(details);
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->currentRow(), 0);
    QVERIFY(row->text().contains(QStringLiteral("T-0001")));
    QVERIFY(row->text().contains(QStringLiteral("无人机")));
    QVERIFY(row->text().contains(QStringLiteral("高度 6.25°")));
    QVERIFY(row->text().contains(QStringLiteral("置信 82%")));
    QVERIFY(details->text().contains(QStringLiteral("26.88°")));
    QVERIFY(details->text().contains(QStringLiteral("6.25°")));
    QVERIFY(details->text().contains(QStringLiteral("RGB+BW")));
    QVERIFY(preview->text().isEmpty());
    QVERIFY(!preview->grab().isNull());

    const QByteArray screenshotPath = qgetenv("DMX_UI_TEST_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        QVERIFY2(panel.grab().save(QString::fromLocal8Bit(screenshotPath)),
                 screenshotPath.constData());
    }
}

void CompactTargetRadarPanelTests::targetInfoPanelShowsFovAndElevation()
{
    TargetRecord target;
    target.time = QDateTime::currentDateTime();
    target.hasElevationAngle = true;
    target.elevationAngleDeg = -4.75;

    TargetInfoPanel panel;
    panel.setTarget(target);

    QLabel *fov = panel.findChild<QLabel*>(QStringLiteral("targetVerticalFovValue"));
    QLabel *elevation = panel.findChild<QLabel*>(QStringLiteral("targetElevationValue"));
    QVERIFY(fov);
    QVERIFY(elevation);
    QCOMPARE(fov->text(), QStringLiteral("26.88 度"));
    QCOMPARE(elevation->text(), QStringLiteral("-4.75 度"));
}

QTEST_MAIN(CompactTargetRadarPanelTests)

#include "compacttargetradarpanel_tests.moc"
