#include "appconfig.h"
#include "radar_ui/targetradarwidget.h"

#include <QtMath>
#include <QtTest>

#ifndef DMX_TEST_BUILD
#error targetradarwidget_tests must exercise the DMX_test build path
#endif

namespace {

enum class MarkerColor
{
    Red,
    Green
};

TargetRecord targetAt(const QString &className, qint64 ageMs,
                      const QString &state = QStringLiteral("tracking"))
{
    TargetRecord target;
    target.id = QStringLiteral("T-TEST");
    target.time = QDateTime::currentDateTime().addMSecs(-ageMs);
    target.lastTime = target.time;
    target.className = className;
    target.state = state;
    target.azimuthDeg = 73.0;
    target.panoY = 1900;
    target.confidence = 0.80;
    return target;
}

QPoint targetPointFor(const QWidget &widget, const TargetRecord &target)
{
    const int side = qMax(128, qMin(widget.width(), widget.height()) - 20);
    const QRect radarRect((widget.width() - side) / 2,
                          (widget.height() - side) / 2,
                          side, side);
    const QPointF center = radarRect.center();
    const double inner = radarRect.width() * 0.105;
    const double outer = radarRect.width() * 0.445;
    const int fullHeight = qMax(1, AppConfig::instance().fullHeight);
    const double yNorm = qBound(0.0,
        (double)target.panoY / (double)qMax(1, fullHeight - 1), 1.0);
    const double normalizedRadius = qBound(0.08, 1.0 - yNorm, 0.96);
    const double radius = inner + normalizedRadius * (outer - inner);
    const double angle = qDegreesToRadians(target.azimuthDeg);
    return QPoint(qRound(center.x() + qSin(angle) * radius),
                  qRound(center.y() - qCos(angle) * radius));
}

bool matchesMarkerColor(const QColor &color, MarkerColor marker)
{
    if (marker == MarkerColor::Red) {
        return color.red() >= 190 && color.red() >= color.green() + 70;
    }
    return color.green() >= 80
        && color.green() >= color.red() + 35
        && color.green() >= color.blue() + 20;
}

int coloredPixels(const QImage &image, const QPoint &center, int radius,
                  MarkerColor marker)
{
    int count = 0;
    for (int y = center.y() - radius; y <= center.y() + radius; ++y) {
        for (int x = center.x() - radius; x <= center.x() + radius; ++x) {
            if (!image.rect().contains(x, y)) continue;
            if (matchesMarkerColor(image.pixelColor(x, y), marker)) ++count;
        }
    }
    return count;
}

QImage renderTarget(TargetRadarWidget *widget, const TargetRecord &target,
                    bool selected)
{
    widget->setTargets(QVector<TargetRecord>() << target);
    widget->setSelectedTarget(selected ? 0 : -1);
    QTest::qWait(35);
    return widget->grab().toImage().convertToFormat(QImage::Format_RGB32);
}

} // namespace

class TargetRadarWidgetTests : public QObject
{
    Q_OBJECT

private slots:
    void classControlsMarkerColor();
    void selectedBirdRemainsGreen();
    void staleTargetUsesHollowMarker();
    void expiredTargetIsNotDrawn();
    void saveScreenshot();
};

void TargetRadarWidgetTests::classControlsMarkerColor()
{
    TargetRadarWidget widget;
    widget.setCompactMode(true);
    widget.resize(420, 420);
    widget.show();

    const TargetRecord drone = targetAt(QStringLiteral("drone"), 200);
    const QPoint point = targetPointFor(widget, drone);
    const QImage droneImage = renderTarget(&widget, drone, false);
    QVERIFY(coloredPixels(droneImage, point, 5, MarkerColor::Red) > 20);
    QCOMPARE(coloredPixels(droneImage, point, 5, MarkerColor::Green), 0);

    const TargetRecord bird = targetAt(QStringLiteral("bird"), 200);
    const QImage birdImage = renderTarget(&widget, bird, false);
    QVERIFY(coloredPixels(birdImage, point, 5, MarkerColor::Green) > 20);
    QCOMPARE(coloredPixels(birdImage, point, 5, MarkerColor::Red), 0);

    const TargetRecord other = targetAt(QStringLiteral("civilian_airliners"), 200);
    const QImage otherImage = renderTarget(&widget, other, false);
    QVERIFY(coloredPixels(otherImage, point, 5, MarkerColor::Green) > 20);
    QCOMPARE(coloredPixels(otherImage, point, 5, MarkerColor::Red), 0);
}

void TargetRadarWidgetTests::selectedBirdRemainsGreen()
{
    TargetRadarWidget widget;
    widget.setCompactMode(true);
    widget.resize(420, 420);
    widget.show();

    const TargetRecord bird = targetAt(QStringLiteral("bird"), 300);
    const QPoint point = targetPointFor(widget, bird);
    const QImage image = renderTarget(&widget, bird, true);
    QVERIFY(coloredPixels(image, point, 5, MarkerColor::Green) > 20);
    QCOMPARE(coloredPixels(image, point, 5, MarkerColor::Red), 0);
}

void TargetRadarWidgetTests::staleTargetUsesHollowMarker()
{
    TargetRadarWidget widget;
    widget.setCompactMode(true);
    widget.resize(420, 420);
    widget.show();

    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    const TargetRecord fresh = targetAt(QStringLiteral("bird"), 200);
    const TargetRecord stale = targetAt(
        QStringLiteral("bird"), holdMs * 4 / 5, QStringLiteral("stale"));
    const QPoint point = targetPointFor(widget, fresh);
    const QImage freshImage = renderTarget(&widget, fresh, false);
    const QImage staleImage = renderTarget(&widget, stale, false);

    const int freshCenterPixels = coloredPixels(freshImage, point, 3, MarkerColor::Green);
    const int staleCenterPixels = coloredPixels(staleImage, point, 3, MarkerColor::Green);
    QVERIFY(freshCenterPixels > 15);
    QVERIFY(staleCenterPixels < freshCenterPixels / 2);
    QVERIFY(coloredPixels(staleImage, point, 10, MarkerColor::Green) > 0);
}

void TargetRadarWidgetTests::expiredTargetIsNotDrawn()
{
    TargetRadarWidget widget;
    widget.setCompactMode(true);
    widget.resize(420, 420);
    widget.show();

    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    const TargetRecord expired = targetAt(QStringLiteral("drone"), holdMs + 1000);
    const QPoint point = targetPointFor(widget, expired);
    const QImage image = renderTarget(&widget, expired, false);
    QCOMPARE(coloredPixels(image, point, 12, MarkerColor::Red), 0);
}

void TargetRadarWidgetTests::saveScreenshot()
{
    const QByteArray screenshotPath = qgetenv("DMX_UI_TEST_SCREENSHOT");
    if (screenshotPath.isEmpty()) return;

    TargetRadarWidget widget;
    widget.setCompactMode(true);
    widget.resize(640, 420);
    widget.show();

    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    TargetRecord drone = targetAt(QStringLiteral("drone"), 250);
    drone.id = QStringLiteral("T-0001");
    drone.azimuthDeg = 45.0;
    TargetRecord bird = targetAt(QStringLiteral("bird"), 800);
    bird.id = QStringLiteral("T-0002");
    bird.azimuthDeg = 145.0;
    TargetRecord stale = targetAt(
        QStringLiteral("civilian_airliners"), holdMs * 4 / 5,
        QStringLiteral("stale"));
    stale.id = QStringLiteral("T-0003");
    stale.azimuthDeg = 255.0;
    widget.setTargets(QVector<TargetRecord>() << drone << bird << stale);
    widget.setSelectedTarget(1);
    QTest::qWait(50);

    QVERIFY2(widget.grab().save(QString::fromLocal8Bit(screenshotPath)),
             screenshotPath.constData());
}

QTEST_MAIN(TargetRadarWidgetTests)

#include "targetradarwidget_tests.moc"
