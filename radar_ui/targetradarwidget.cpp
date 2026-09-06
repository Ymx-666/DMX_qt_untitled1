#include "targetradarwidget.h"

#include "appconfig.h"
#include "polarpanoramaprojector.h"

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QTimer>
#include <QtMath>

static double norm360(double a)
{
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

static double shortestAngleDelta(double from, double to)
{
    double diff = norm360(to) - norm360(from);
    if (diff > 180.0) diff -= 360.0;
    if (diff < -180.0) diff += 360.0;
    return diff;
}

static double clamp01(double v)
{
    return qBound(0.0, v, 1.0);
}

#ifdef DMX_TEST_BUILD
static QColor testTargetClassColor(const TargetRecord &target)
{
    const QString className = target.className.trimmed().toLower();
    if (className == QStringLiteral("drone")) return QColor(255, 60, 52);
    return QColor(58, 214, 116);
}

static void drawTestTargetLegend(QPainter &p, const QRect &bounds, bool compact)
{
    p.save();
    QFont legendFont = p.font();
    legendFont.setPointSize(compact ? 7 : 9);
    p.setFont(legendFont);

    const qreal dotRadius = compact ? 2.5 : 3.5;
    const qreal rowHeight = compact ? 12.0 : 16.0;
    const qreal x = bounds.left() + (compact ? 5.0 : 8.0);
    const qreal y = bounds.top() + (compact ? 5.0 : 8.0);
    const qreal textWidth = compact ? 34.0 : 48.0;

    auto drawLegendItem = [&](qreal itemX, qreal itemY, const QColor &color,
                              const QString &text, bool filled) {
        p.setPen(QPen(color, compact ? 1.2 : 1.6));
        p.setBrush(filled ? QBrush(color) : Qt::NoBrush);
        const QPointF center(itemX + dotRadius, itemY + rowHeight * 0.5);
        p.drawEllipse(center, dotRadius, dotRadius);
        p.setPen(QColor(220, 236, 232, 225));
        p.drawText(QRectF(itemX + dotRadius * 2.0 + 4.0, itemY,
                          textWidth, rowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, text);
    };

    const qreal secondColumn = x + (compact ? 58.0 : 78.0);
    drawLegendItem(x, y, QColor(255, 60, 52), QStringLiteral("无人机"), true);
    drawLegendItem(secondColumn, y, QColor(58, 214, 116), QStringLiteral("其他"), true);
    drawLegendItem(x, y + rowHeight, QColor(205, 224, 219), QStringLiteral("实时"), true);
    drawLegendItem(secondColumn, y + rowHeight, QColor(205, 224, 219),
                   QStringLiteral("待清理"), false);
    p.restore();
}
#endif

static QImage shiftPanoramaByDegrees(const QImage &src, double deg)
{
    if (src.isNull() || src.width() <= 1) return src;

    QImage in = src.convertToFormat(QImage::Format_RGB32);
    QImage out(in.size(), QImage::Format_RGB32);
    if (out.isNull()) return QImage();

    const int w = in.width();
    const int h = in.height();
    const int shift = qRound(norm360(deg) / 360.0 * (double)w) % w;
    if (shift == 0) return in;

    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) d[x] = s[(x + shift) % w];
    }
    return out;
}

TargetRadarWidget::TargetRadarWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(640, 640);
    setMouseTracking(true);
    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(16);
    connect(m_scanTimer, &QTimer::timeout, this, &TargetRadarWidget::advanceScanAnimation);
    m_scanTimer->start();
}

void TargetRadarWidget::setPanoramaBackground(const QImage &panorama, bool bwNeeds180Align)
{
    m_panorama = panorama;
    m_bwNeeds180Align = bwNeeds180Align;
    rebuildPolar();
    update();
}

void TargetRadarWidget::setCompactMode(bool compact)
{
    if (m_compactMode == compact) return;
    m_compactMode = compact;
    if (m_compactMode) {
        setMinimumSize(150, 150);
    } else {
        setMinimumSize(640, 640);
    }
    rebuildPolar();
    update();
}

void TargetRadarWidget::setTargets(const QVector<TargetRecord> &targets)
{
    m_targets = targets;
    update();
}

void TargetRadarWidget::setSelectedTarget(int index)
{
    m_selected = index;
    update();
}

void TargetRadarWidget::setScanAngle(double angleDeg)
{
    m_rawScanAngle = norm360(angleDeg);
    m_targetScanAngle = rawToDisplayAngle(m_rawScanAngle);
}

void TargetRadarWidget::setNorthOffsetDeg(double rawNorthDeg)
{
    const double normalized = norm360(rawNorthDeg);
    if (qAbs(shortestAngleDelta(m_northOffsetDeg, normalized)) < 0.001) return;

    m_northOffsetDeg = normalized;
    m_targetScanAngle = rawToDisplayAngle(m_rawScanAngle);
    m_displayScanAngle = m_targetScanAngle;
    rebuildPolar();
    update();
    emit northOffsetChanged(m_northOffsetDeg);
}

double TargetRadarWidget::northOffsetDeg() const
{
    return m_northOffsetDeg;
}

void TargetRadarWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(9, 12, 15));

    const QRect rr = radarRect();
    const QPointF c = rr.center();
    const double outer = rr.width() * (m_compactMode ? 0.445 : 0.475);
    const double inner = rr.width() * 0.105;

    if (!m_polar.isNull()) {
        p.drawImage(rr, m_polar);
    } else {
        p.setBrush(QColor(16, 22, 25));
        p.setPen(Qt::NoPen);
        p.drawEllipse(c, outer, outer);
    }

    QPen gridPen(QColor(125, 210, 185, 150));
    gridPen.setWidthF(m_compactMode ? 0.8 : 1.1);
    p.setPen(gridPen);
    p.setBrush(Qt::NoBrush);
    const double rings[] = {inner, outer * 0.38, outer * 0.58, outer * 0.78, outer};
    for (double r : rings) p.drawEllipse(c, r, r);
    for (int deg = 0; deg < 360; deg += (m_compactMode ? 30 : 15)) {
        const double a = qDegreesToRadians((double)deg);
        const QPointF p1(c.x() + qSin(a) * inner, c.y() - qCos(a) * inner);
        const QPointF p2(c.x() + qSin(a) * outer, c.y() - qCos(a) * outer);
        p.drawLine(p1, p2);
    }

    QFont tickFont = font();
    tickFont.setPointSize(m_compactMode ? 7 : 9);
    p.setFont(tickFont);
    const int tickStep = m_compactMode ? 15 : 5;
    for (int deg = 0; deg < 360; deg += tickStep) {
        const bool major = deg % (m_compactMode ? 90 : 30) == 0;
        const bool medium = deg % 30 == 0;
        const double tickLen = m_compactMode
            ? (major ? 7.0 : (medium ? 5.0 : 3.0))
            : (major ? 14.0 : (medium ? 9.0 : 5.0));
        const double a = qDegreesToRadians((double)deg);
        const QPointF p1(c.x() + qSin(a) * (outer + 4.0), c.y() - qCos(a) * (outer + 4.0));
        const QPointF p2(c.x() + qSin(a) * (outer + 4.0 + tickLen), c.y() - qCos(a) * (outer + 4.0 + tickLen));
        QPen tickPen(QColor(185, 230, 218, major ? 220 : (medium ? 170 : 110)));
        tickPen.setWidthF(major ? (m_compactMode ? 1.2 : 1.6) : 0.8);
        p.setPen(tickPen);
        p.drawLine(p1, p2);

        if (!major) continue;
        QString label = QString::number(deg);
        if (deg == 0) label = m_compactMode ? QStringLiteral("北") : QStringLiteral("0 北");
        else if (deg == 90) label = m_compactMode ? QStringLiteral("东") : QStringLiteral("90 东");
        else if (deg == 180) label = m_compactMode ? QStringLiteral("南") : QStringLiteral("180 南");
        else if (deg == 270) label = m_compactMode ? QStringLiteral("西") : QStringLiteral("270 西");

        const double labelRadius = outer + (m_compactMode ? 12.0 : 20.0);
        const QPointF lp(c.x() + qSin(a) * labelRadius, c.y() - qCos(a) * labelRadius);
        const double labelWidth = m_compactMode ? 22.0 : 52.0;
        const double labelHeight = m_compactMode ? 14.0 : 20.0;
        const QRectF labelRect(lp.x() - labelWidth / 2.0, lp.y() - labelHeight / 2.0,
                               labelWidth, labelHeight);
        p.setPen(QColor(220, 241, 235, 230));
        p.drawText(labelRect, Qt::AlignCenter, label);
    }

    QPainterPath sweep;
    sweep.moveTo(c);
    const double start = m_displayScanAngle - 9.0;
    for (int i = 0; i <= 18; i += 2) {
        const double a = qDegreesToRadians(start + i);
        sweep.lineTo(c.x() + qSin(a) * outer, c.y() - qCos(a) * outer);
    }
    sweep.closeSubpath();
    p.fillPath(sweep, QColor(80, 255, 135, 45));
    QPen scanPen(QColor(90, 255, 145));
    scanPen.setWidthF(m_compactMode ? 1.5 : 3.0);
    p.setPen(scanPen);
    const double sa = qDegreesToRadians(m_displayScanAngle);
    p.drawLine(c, QPointF(c.x() + qSin(sa) * outer, c.y() - qCos(sa) * outer));

    p.setBrush(QColor(12, 16, 18));
    p.setPen(QPen(QColor(130, 230, 205), 1.2));
    p.drawEllipse(c, inner - 8.0, inner - 8.0);

    // Detection target layer. The layer is drawn after the background, grid and
    // scan beam so current algorithm decisions remain visually independent.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        const TargetRecord &t = m_targets.at(i);
#ifdef DMX_ADVANCED_DETECTION
        if (t.staticSuppressed) continue;
#endif
        const QDateTime last = t.lastTime.isValid() ? t.lastTime : t.time;
        const qint64 ageMs = last.isValid()
            ? qMax<qint64>(0, last.msecsTo(QDateTime::fromMSecsSinceEpoch(nowMs)))
            : holdMs;
#ifdef DMX_ADVANCED_DETECTION
        if (ageMs > holdMs) continue;
#endif
        const QPointF pt = targetPoint(t);
        const double freshness = 1.0 - clamp01((double)ageMs / (double)holdMs);
        const double confidence = clamp01(t.confidence);
        const double dot = m_compactMode
            ? 3.0 + confidence * 4.0
            : 6.0 + confidence * 10.0;
        const double ring = m_compactMode
            ? dot + 3.0 + confidence * 3.0
            : dot + 7.0 + confidence * 7.0;
#ifdef DMX_TEST_BUILD
        const bool selected = i == m_selected;
        const bool staleVisual = freshness < 0.34
            || t.state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0;
        const bool confirmed = t.state.compare(
            QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0;
        const bool justDetected = ageMs <= 1400;
        QColor col = testTargetClassColor(t);
        col.setAlpha(staleVisual ? 135 : (int)qRound(205 + freshness * 50.0));

        QPen dotPen(col, confirmed ? 2.4 : 1.8);
        if (staleVisual) dotPen.setStyle(Qt::DashLine);
        p.setPen(dotPen);
        p.setBrush(staleVisual ? Qt::NoBrush : QBrush(col));
        p.drawEllipse(pt, dot, dot);

        QColor ringColor = col;
        ringColor.setAlpha(staleVisual ? 105 : 180);
        QPen ringPen(ringColor, confirmed ? 2.0 : 1.4,
                     staleVisual ? Qt::DashLine : Qt::SolidLine);
        p.setPen(ringPen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(pt, ring, ring);
        if (confirmed && !staleVisual) {
            ringColor.setAlpha(115);
            p.setPen(QPen(ringColor, 1.0));
            p.drawEllipse(pt, ring + (m_compactMode ? 2.5 : 4.0),
                          ring + (m_compactMode ? 2.5 : 4.0));
        }

        if (justDetected && !staleVisual) {
            const double phase = (double)(nowMs % 900) / 900.0;
            const double pulseRadius = ring + (m_compactMode ? 2.0 : 4.0)
                + phase * (m_compactMode ? 7.0 : 12.0);
            QColor pulseColor = testTargetClassColor(t);
            pulseColor.setAlpha((int)qRound((1.0 - phase) * 150.0));
            p.setPen(QPen(pulseColor, m_compactMode ? 1.2 : 1.8));
            p.drawEllipse(pt, pulseRadius, pulseRadius);
        }

        if (selected) {
            const double focusRadius = ring + (m_compactMode ? 5.0 : 8.0);
            p.setPen(QPen(QColor(85, 230, 199, 245),
                          m_compactMode ? 2.0 : 2.8));
            p.drawEllipse(pt, focusRadius, focusRadius);
        }
#else
        QColor col(255, (int)qRound(58 + (1.0 - freshness) * 118.0),
                   (int)qRound(42 + (1.0 - freshness) * 118.0));
        col.setAlpha((int)qRound(140 + freshness * 115.0));
        if (i == m_selected) col = QColor(255, 64, 36);
        p.setBrush(col);
        QPen dotPen(col.lighter(135), i == m_selected ? 2.8 : 1.7);
        if (t.state.compare(QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0) {
            dotPen.setWidthF(i == m_selected ? 3.2 : 2.4);
        }
        const bool staleVisual = freshness < 0.34
            || t.state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0;
        if (staleVisual) {
            col.setAlpha(qMin(col.alpha(), 150));
            p.setBrush(col);
            dotPen.setStyle(Qt::DashLine);
        }
        p.setPen(dotPen);
        p.drawEllipse(pt, dot, dot);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(pt, i == m_selected ? ring + 4.0 : ring,
                      i == m_selected ? ring + 4.0 : ring);
#endif
        if (!m_compactMode) {
            p.setPen(QColor(245, 235, 230, i == m_selected ? 255 : 215));
            p.drawText(pt + QPointF(16, -8), t.id);
        }
    }
#ifdef DMX_TEST_BUILD
    drawTestTargetLegend(p, rect(), m_compactMode);
#endif
}

void TargetRadarWidget::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->pos();
    if (event->button() == Qt::RightButton && radarRect().contains(event->pos())) {
        const double raw = pointAzimuth(pos);
        QMenu menu(this);
        QAction *setNorth = menu.addAction(QStringLiteral("将该方向设为显示正北（设备 %1°）").arg(raw, 0, 'f', 1));
        QAction *resetNorth = menu.addAction(QStringLiteral("清除本次正北校准"));
        QAction *chosen = menu.exec(event->globalPos());
        if (chosen == setNorth) setNorthOffsetDeg(raw);
        else if (chosen == resetNorth) setNorthOffsetDeg(0.0);
        return;
    }

    if (event->button() != Qt::LeftButton) return;

    int best = -1;
    double bestD2 = 26.0 * 26.0;
#ifdef DMX_ADVANCED_DETECTION
    const QDateTime now = QDateTime::currentDateTime();
    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
#endif
    for (int i = 0; i < m_targets.size(); ++i) {
#ifdef DMX_ADVANCED_DETECTION
        if (m_targets.at(i).staticSuppressed) continue;
        const QDateTime last = m_targets.at(i).lastTime.isValid()
            ? m_targets.at(i).lastTime
            : m_targets.at(i).time;
        if (last.isValid() && last.msecsTo(now) > holdMs) continue;
#endif
        const QPointF pt = targetPoint(m_targets.at(i));
        const double dx = pt.x() - pos.x();
        const double dy = pt.y() - pos.y();
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = i;
        }
    }
    if (best >= 0) {
        emit targetClicked(best);
        return;
    }
    if (radarRect().contains(event->pos())) emit azimuthClicked(pointAzimuth(pos));
}

void TargetRadarWidget::resizeEvent(QResizeEvent *)
{
    rebuildPolar();
}

void TargetRadarWidget::advanceScanAnimation()
{
    const double diff = shortestAngleDelta(m_displayScanAngle, m_targetScanAngle);
#ifdef DMX_TEST_BUILD
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool targetPulseActive = false;
    for (const TargetRecord &target : m_targets) {
        const QDateTime last = target.lastTime.isValid() ? target.lastTime : target.time;
        const qint64 ageMs = last.isValid()
            ? last.msecsTo(QDateTime::fromMSecsSinceEpoch(nowMs))
            : -1;
        if (ageMs >= 0 && ageMs <= 1400) {
            targetPulseActive = true;
            break;
        }
    }
    if (qAbs(diff) < 0.05) {
        if (targetPulseActive) update();
        return;
    }
#else
    if (qAbs(diff) < 0.05) return;
#endif

    const double step = qBound(-5.0, diff * 0.35, 5.0);
    m_displayScanAngle = norm360(m_displayScanAngle + step);
    update();
}

void TargetRadarWidget::rebuildPolar()
{
    const QRect rr = radarRect();
    const int size = qMax(m_compactMode ? 128 : 320, qMin(rr.width(), rr.height()));
    if (m_panorama.isNull()) {
        m_polar = QImage();
        return;
    }
    QImage pano = m_bwNeeds180Align ? PolarPanoramaProjector::rotateAzimuth180(m_panorama) : m_panorama;
    if (!qFuzzyIsNull(m_northOffsetDeg)) pano = shiftPanoramaByDegrees(pano, m_northOffsetDeg);
    const double outerRatio = m_compactMode ? 0.445 : 0.475;
    m_polar = PolarPanoramaProjector::projectHeightPolar(
        pano, size, (int)(size * 0.105), (int)(size * outerRatio));
}

QRect TargetRadarWidget::radarRect() const
{
    const int minimumSide = m_compactMode ? 128 : 320;
    // Full-size cardinal labels are wider than compact labels. Reserve enough
    // paint space so "270 西" and "90 东" are not clipped at panel edges.
    const int margin = m_compactMode ? 20 : 48;
    const int side = qMax(minimumSide, qMin(width(), height()) - margin);
    return QRect((width() - side) / 2, (height() - side) / 2, side, side);
}

double TargetRadarWidget::targetRadius(const TargetRecord &target) const
{
    const int fullHeight = qMax(1, AppConfig::instance().fullHeight);
    if (target.panoY <= 0) return 0.72;
    const double yNorm = clamp01((double)target.panoY / (double)qMax(1, fullHeight - 1));
    return qBound(0.08, 1.0 - yNorm, 0.96);
}

QPointF TargetRadarWidget::targetPoint(const TargetRecord &target) const
{
    return polarPoint(rawToDisplayAngle(target.azimuthDeg), targetRadius(target));
}

QPointF TargetRadarWidget::polarPoint(double angleDeg, double normalizedRadius) const
{
    const QRect rr = radarRect();
    const QPointF c = rr.center();
    const double inner = rr.width() * 0.105;
    const double outer = rr.width() * (m_compactMode ? 0.445 : 0.475);
    const double r = inner + qBound(0.0, normalizedRadius, 1.0) * (outer - inner);
    const double a = qDegreesToRadians(norm360(angleDeg));
    return QPointF(c.x() + qSin(a) * r, c.y() - qCos(a) * r);
}

double TargetRadarWidget::pointAzimuth(const QPointF &p) const
{
    return displayToRawAngle(pointDisplayAzimuth(p));
}

double TargetRadarWidget::rawToDisplayAngle(double rawAngle) const
{
    return norm360(rawAngle - m_northOffsetDeg);
}

double TargetRadarWidget::displayToRawAngle(double displayAngle) const
{
    return norm360(displayAngle + m_northOffsetDeg);
}

double TargetRadarWidget::pointDisplayAzimuth(const QPointF &p) const
{
    const QPointF c = radarRect().center();
    const double dx = p.x() - c.x();
    const double dy = c.y() - p.y();
    return norm360(qRadiansToDegrees(qAtan2(dx, dy)));
}
