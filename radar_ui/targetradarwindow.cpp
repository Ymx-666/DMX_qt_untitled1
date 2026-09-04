#include "targetradarwindow.h"

#include "appconfig.h"
#include "imageviewerdialog.h"
#include "manualnegativestore.h"
#include "polarpanoramaprojector.h"
#include "targetinfopanel.h"
#include "targetlistwidget.h"
#include "targetpreviewpanel.h"
#include "targetradarwidget.h"

#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRect>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

static double norm360Local(double a)
{
    while (a < 0.0) a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

static QString defaultPanoramaPath()
{
    return QStringLiteral("/mnt/dmx4t/DMX_yangben/20260626/_analysis/new_radar_ui_mock_bw11/BW_20260626_113856_0_AB_pano_8192x512_bw_rot180.jpg");
}

static double angleDistance(double a, double b)
{
    double diff = qAbs(norm360Local(a) - norm360Local(b));
    if (diff > 180.0) diff = 360.0 - diff;
    return diff;
}

#ifndef DMX_ADVANCED_DETECTION
static int targetVerticalTolerance()
{
    return qMax(64, AppConfig::instance().fullHeight / 32);
}
#else
static qint64 targetVisibleRetentionMs()
{
    return qMax<qint64>(2000, (qint64)AppConfig::instance().detectRadarHoldMs);
}

static qint64 targetAssociationRetentionMs()
{
    return qMax<qint64>(120000, (qint64)AppConfig::instance().detectRadarHoldMs * 6);
}

static QString normalizedStream(const QString &stream)
{
    const QString upper = stream.trimmed().toUpper();
    if (upper.contains(QStringLiteral("RGB")) && upper.contains(QStringLiteral("BW"))) {
        return QStringLiteral("RGB+BW");
    }
    if (upper == QStringLiteral("GRAY")) return QStringLiteral("BW");
    return upper;
}

static QString normalizedTargetClass(const QString &className)
{
    QString normalized = className.trimmed().toLower();
    normalized.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (normalized == QStringLiteral("airliner")
        || normalized == QStringLiteral("civilian_airliner")) {
        return QStringLiteral("civilian_airliners");
    }
    return normalized;
}

static bool isKnownSingleStream(const QString &stream)
{
    return stream == QStringLiteral("RGB") || stream == QStringLiteral("BW");
}

static QString mergedStream(const QString &oldStream, const QString &newStream)
{
    const QString oldNormalized = normalizedStream(oldStream);
    const QString newNormalized = normalizedStream(newStream);
    if (oldNormalized == newNormalized) return oldNormalized;
    if ((oldNormalized.contains(QStringLiteral("RGB")) || newNormalized == QStringLiteral("RGB"))
        && (oldNormalized.contains(QStringLiteral("BW")) || newNormalized == QStringLiteral("BW"))) {
        return QStringLiteral("RGB+BW");
    }
    return newNormalized.isEmpty() ? oldNormalized : newNormalized;
}

static QString sourceFrameKey(const QString &path)
{
    QString base = QFileInfo(path).completeBaseName();
    int marker = base.indexOf(QStringLiteral("_direct_"));
    if (marker > 0) return base.left(marker);
    marker = base.lastIndexOf(QStringLiteral("_px"));
    if (marker > 0) return base.left(marker);
    return base;
}

static qint64 sourceFrameSequence(const QString &frameKey, bool *ok)
{
    if (ok) *ok = false;
    int pos = frameKey.size() - 1;
    while (pos >= 0 && frameKey.at(pos).isDigit()) --pos;
    if (pos == frameKey.size() - 1) return 0;
    return frameKey.mid(pos + 1).toLongLong(ok);
}

static int bitDistance(quint64 a, quint64 b)
{
    quint64 value = a ^ b;
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

static quint64 targetAppearanceHash(const TargetRecord &target, bool *ok)
{
    if (ok) *ok = false;
    if (!target.hasRoiBox || target.imagePath.isEmpty()) return 0;

    QImage source(target.imagePath);
    if (source.isNull()) return 0;
    const QRect imageRect(0, 0, source.width(), source.height());
    const QRect box(QPoint(target.roiBoxX1, target.roiBoxY1),
                    QPoint(target.roiBoxX2, target.roiBoxY2));
    const QRect normalizedBox = box.normalized().intersected(imageRect);
    if (normalizedBox.width() < 2 || normalizedBox.height() < 2) return 0;

    const QPoint center = normalizedBox.center();
    const int side = qMax(32, qMax(normalizedBox.width(), normalizedBox.height()) * 3);
    const QRect patchRect(center.x() - side / 2, center.y() - side / 2, side, side);
    QImage patch = source.copy(patchRect.intersected(imageRect))
        .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
    if (patch.width() != 9 || patch.height() != 8) return 0;

    quint64 hash = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(patch.constScanLine(y));
        for (int x = 0; x < 8; ++x, ++bit) {
            if (qGray(line[x]) > qGray(line[x + 1])) hash |= (quint64(1) << bit);
        }
    }
    if (ok) *ok = true;
    return hash;
}

static TargetRecord prepareAssociationFeatures(const TargetRecord &target)
{
    TargetRecord prepared = target;
    prepared.stream = normalizedStream(target.stream);
    prepared.className = normalizedTargetClass(target.className);
    prepared.classConfidence = qMax(target.classConfidence, target.confidence);
    prepared.lastStream = isKnownSingleStream(prepared.stream)
        ? prepared.stream
        : normalizedStream(target.lastStream);
    prepared.frameKey = sourceFrameKey(target.imagePath);
    prepared.imageConfidence = qMax(target.imageConfidence, target.confidence);
    if (target.hasRoiBox) {
        const double width = qMax(1, qAbs(target.roiBoxX2 - target.roiBoxX1));
        const double height = qMax(1, qAbs(target.roiBoxY2 - target.roiBoxY1));
        prepared.boxAspect = width / height;
        prepared.boxArea = width * height;
    }
    if (!prepared.hasAppearanceHash) {
        prepared.appearanceHash = targetAppearanceHash(prepared, &prepared.hasAppearanceHash);
    }
    return prepared;
}

static double positiveRatio(double a, double b)
{
    if (a <= 0.0 || b <= 0.0) return 1.0;
    return qMax(a, b) / qMax(1.0e-6, qMin(a, b));
}

struct TargetMatch
{
    bool eligible = false;
    double cost = 1.0e9;
    QString mode;
    qint64 dtMs = 0;
    double angleDiff = 0.0;
    int yDiff = 0;
    double areaRatio = 1.0;
    double aspectRatio = 1.0;
    int hashDistance = -1;
};

static TargetMatch evaluateTargetMatch(const TargetRecord &existing, const TargetRecord &incoming)
{
    TargetMatch match;
    const QDateTime existingTime = existing.lastTime.isValid() ? existing.lastTime : existing.time;
    const QDateTime incomingTime = incoming.lastTime.isValid() ? incoming.lastTime : incoming.time;
    if (!existingTime.isValid() || !incomingTime.isValid()) return match;

    match.dtMs = qAbs(existingTime.msecsTo(incomingTime));
    if (match.dtMs > targetAssociationRetentionMs()) return match;
    if (!existing.frameKey.isEmpty() && existing.frameKey == incoming.frameKey) return match;

    match.angleDiff = angleDistance(existing.azimuthDeg, incoming.azimuthDeg);
    match.yDiff = qAbs(existing.panoY - incoming.panoY);
    match.areaRatio = positiveRatio(existing.boxArea, incoming.boxArea);
    match.aspectRatio = positiveRatio(existing.boxAspect, incoming.boxAspect);

    const QString existingClass = normalizedTargetClass(existing.className);
    const QString incomingClass = normalizedTargetClass(incoming.className);
    const bool classConflict = !existingClass.isEmpty()
        && !incomingClass.isEmpty()
        && existingClass != incomingClass;

    const QString existingLastStream = normalizedStream(existing.lastStream);
    const QString incomingLastStream = normalizedStream(incoming.lastStream);
    const bool crossStream = isKnownSingleStream(existingLastStream)
        && isKnownSingleStream(incomingLastStream)
        && existingLastStream != incomingLastStream;

    if (crossStream) {
        bool existingSequenceOk = false;
        bool incomingSequenceOk = false;
        const qint64 existingSequence = sourceFrameSequence(existing.frameKey, &existingSequenceOk);
        const qint64 incomingSequence = sourceFrameSequence(incoming.frameKey, &incomingSequenceOk);
        const bool pairedFrame = existingSequenceOk
            && incomingSequenceOk
            && existingSequence == incomingSequence
            && match.dtMs <= 1200;
        const bool halfRevolution = match.dtMs >= 2500 && match.dtMs <= 6500;
        if (!pairedFrame && !halfRevolution) return match;

        // A paired RGB/BW observation can classify the same object differently.
        // Only accept that conflict when the registered positions nearly coincide.
        if (classConflict
            && (!pairedFrame || match.angleDiff > 0.45 || match.yDiff > 100
                || match.areaRatio > 6.0 || match.aspectRatio > 4.0)) {
            return match;
        }

        const double angleTolerance = pairedFrame ? 1.4 : 2.2;
        const int yTolerance = pairedFrame ? 300 : 420;
        if (match.angleDiff > angleTolerance || match.yDiff > yTolerance) return match;
        if (match.areaRatio > 14.0 || match.aspectRatio > 6.0) return match;

        match.eligible = true;
        match.mode = pairedFrame
            ? (classConflict
                ? QStringLiteral("rgb_bw_pair_class_vote")
                : QStringLiteral("rgb_bw_pair"))
            : QStringLiteral("rgb_bw_half");
        match.cost = match.angleDiff / angleTolerance
            + (double)match.yDiff / (double)yTolerance
            + (pairedFrame
                ? (double)match.dtMs / 1200.0
                : qAbs((double)match.dtMs - 4000.0) / 4000.0)
            + std::log(match.areaRatio) * 0.18
            + std::log(match.aspectRatio) * 0.22;
        return match;
    }

    if (existingLastStream != incomingLastStream) return match;

    if (match.dtMs < 1500) {
        bool existingSequenceOk = false;
        bool incomingSequenceOk = false;
        const qint64 existingSequence = sourceFrameSequence(existing.frameKey, &existingSequenceOk);
        const qint64 incomingSequence = sourceFrameSequence(incoming.frameKey, &incomingSequenceOk);
        if (existingSequenceOk && incomingSequenceOk
            && qAbs(existingSequence - incomingSequence) > 2) {
            return match;
        }

        const bool hasAppearance = existing.hasAppearanceHash && incoming.hasAppearanceHash;
        if (hasAppearance) {
            match.hashDistance = bitDistance(existing.appearanceHash, incoming.appearanceHash);
        }

        const bool tightGeometry = match.angleDiff <= 0.55
            && match.yDiff <= 120
            && match.areaRatio <= 5.0
            && match.aspectRatio <= 3.5;
        const bool veryTightGeometry = match.angleDiff <= 0.20
            && match.yDiff <= 48
            && match.areaRatio <= 2.5
            && match.aspectRatio <= 2.5;
        const bool appearanceCompatible = hasAppearance
            ? (match.hashDistance <= 20 || veryTightGeometry)
            : (match.angleDiff <= 0.25
                && match.yDiff <= 64
                && match.areaRatio <= 3.0
                && match.aspectRatio <= 2.5);

        // Adjacent source frames overlap. Merge only when geometry and appearance
        // strongly agree, while same-frame boxes remain independent above.
        if (!tightGeometry || !appearanceCompatible) return match;
        if (classConflict && (!hasAppearance || match.hashDistance > 14 || !veryTightGeometry)) {
            return match;
        }

        match.eligible = true;
        match.mode = classConflict
            ? QStringLiteral("adjacent_frame_class_vote")
            : QStringLiteral("adjacent_frame");
        match.cost = match.angleDiff / 0.55
            + (double)match.yDiff / 120.0
            + (double)match.dtMs / 1500.0
            + std::log(match.areaRatio) * 0.22
            + std::log(match.aspectRatio) * 0.28;
        if (match.hashDistance >= 0) match.cost += (double)match.hashDistance / 32.0;
        return match;
    }

    if (classConflict) return match;
    const int revolutionCount = qMax(1, qRound((double)match.dtMs / 8000.0));
    const qint64 cycleResidualMs = qAbs(match.dtMs - (qint64)revolutionCount * 8000);
    if (cycleResidualMs > 2600) return match;

    const double dtSec = (double)match.dtMs / 1000.0;
    const double angleTolerance = 1.0 + qMin(2.0, dtSec * 0.035);
    const int yTolerance = 96 + qMin(320, qRound(dtSec * 4.0));
    if (match.angleDiff > angleTolerance || match.yDiff > yTolerance) return match;
    if (match.areaRatio > 10.0 || match.aspectRatio > 5.0) return match;

    if (existing.hasAppearanceHash && incoming.hasAppearanceHash) {
        match.hashDistance = bitDistance(existing.appearanceHash, incoming.appearanceHash);
        const bool appearanceCompatible = match.hashDistance <= 20
            || (match.hashDistance <= 26 && match.angleDiff <= 1.2 && match.yDiff <= 180)
            || (match.angleDiff <= 0.65 && match.yDiff <= 100);
        if (!appearanceCompatible) return match;
    }

    match.eligible = true;
    match.mode = QStringLiteral("revisit");
    match.cost = match.angleDiff / angleTolerance
        + (double)match.yDiff / (double)yTolerance
        + (double)cycleResidualMs / 2600.0
        + std::log(match.areaRatio) * 0.20
        + std::log(match.aspectRatio) * 0.25;
    if (match.hashDistance >= 0) match.cost += (double)match.hashDistance / 32.0;
    return match;
}
#endif

static QString classifyTargetState(const TargetRecord &target, qint64 nowMs)
{
    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    const QDateTime last = target.lastTime.isValid() ? target.lastTime : target.time;
    const qint64 ageMs = last.isValid()
        ? qMax<qint64>(0, last.msecsTo(QDateTime::fromMSecsSinceEpoch(nowMs)))
        : holdMs;
    if (ageMs > holdMs * 2 / 3) return QStringLiteral("stale");
    if (target.hits >= 3 && target.confidence >= 0.45) return QStringLiteral("confirmed");
    if (target.hits >= 2 || target.confidence >= 0.55) return QStringLiteral("tracking");
    return QStringLiteral("new");
}

TargetRadarWindow::TargetRadarWindow(const QString &panoramaPath, QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("目标雷达界面测试"));
    resize(1920, 1080);
#ifdef DMX_ADVANCED_DETECTION
    QString feedbackError;
    const QString feedbackRoot = ManualNegativeStore::defaultRootPath();
    if (!m_manualNegativeFeedback.reload(feedbackRoot, &feedbackError)) {
        qWarning("manual negative feedback load failed: %s", qPrintable(feedbackError));
    } else {
        qInfo("manual negative feedback loaded: samples=%d root=%s",
              m_manualNegativeFeedback.sampleCount(), qPrintable(feedbackRoot));
    }
#endif

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 10);
    QHBoxLayout *top = new QHBoxLayout();
    QLabel *title = new QLabel(QStringLiteral("目标雷达控制台"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px; font-weight:700;"));
    m_mode = new QComboBox(this);
    m_mode->setObjectName(QStringLiteral("backgroundModeCombo"));
    m_mode->addItems(QStringList() << QStringLiteral("黑白高度极坐标") << QStringLiteral("彩色背景") << QStringLiteral("融合背景"));
    m_northLabel = new QLabel(this);
    m_northLabel->setMinimumWidth(180);
    m_setNorthButton = new QPushButton(QStringLiteral("设为正北"), this);
    m_setNorthButton->setEnabled(false);
    m_setNorthButton->setToolTip(QStringLiteral("先左键点击雷达图上的参考方位，再将该方位设为本次显示正北。"));
    m_resetNorthButton = new QPushButton(QStringLiteral("正北归零"), this);
    m_resetNorthButton->setToolTip(QStringLiteral("清除本次显示正北校准，恢复设备原始方位。"));
    top->addWidget(title, 1);
    top->addWidget(new QLabel(QStringLiteral("背景模式"), this));
    top->addWidget(m_mode);
    top->addSpacing(12);
    top->addWidget(m_northLabel);
    top->addWidget(m_setNorthButton);
    top->addWidget(m_resetNorthButton);
    root->addLayout(top);

    QHBoxLayout *body = new QHBoxLayout();
    m_list = new TargetListWidget(this);
    m_radar = new TargetRadarWidget(this);
    m_radar->setToolTip(QStringLiteral("右键点击雷达图方位可直接设为本次显示正北。"));
    QWidget *right = new QWidget(this);
    QVBoxLayout *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_info = new TargetInfoPanel(right);
    m_preview = new TargetPreviewPanel(right);
    rightLayout->addWidget(m_info);
    rightLayout->addWidget(m_preview, 1);

    body->addWidget(m_list, 0);
    body->addWidget(m_radar, 1);
    body->addWidget(right, 0);
    m_list->setMinimumWidth(310);
    m_list->setMaximumWidth(370);
    right->setMinimumWidth(320);
    right->setMaximumWidth(390);
    body->setStretch(0, 0);
    body->setStretch(1, 1);
    body->setStretch(2, 0);
    root->addLayout(body, 1);

    setStyleSheet(QStringLiteral(
        "QWidget{background:#12171b;color:#d7e0e3;font-family:Sans Serif;}"
        "QListWidget{background:#1b2227;border:1px solid #41505a;}"
        "QListWidget::item{background:#20272c;border:1px solid #4c5962;margin:4px;padding:8px;}"
        "QListWidget::item:selected{border:2px solid #55e6c7;}"
        "QComboBox{background:#20272c;border:1px solid #596872;padding:5px;}"
        "QPushButton{background:#20272c;border:1px solid #596872;padding:6px 10px;}"
        "QPushButton:disabled{color:#7c878c;border-color:#36434a;}"
        "QLabel{color:#d7e0e3;}"
    ));

    buildMockTargets();
    refreshTargetViews();
    loadPanorama(panoramaPath.trimmed().isEmpty() ? defaultPanoramaPath() : panoramaPath);
    applySelection();

    connect(m_list, &TargetListWidget::targetSelected, this, &TargetRadarWindow::selectTarget);
    connect(m_list, &TargetListWidget::targetImageRequested, this, &TargetRadarWindow::openTargetImage);
#ifdef DMX_ADVANCED_DETECTION
    connect(m_list, &TargetListWidget::falsePositiveRequested,
            this, &TargetRadarWindow::markFalsePositive);
#endif
    connect(m_radar, &TargetRadarWidget::targetClicked, this, &TargetRadarWindow::selectTarget);
    connect(m_radar, &TargetRadarWidget::azimuthClicked, this, &TargetRadarWindow::handleAzimuth);
    connect(m_radar, &TargetRadarWidget::northOffsetChanged, this, &TargetRadarWindow::updateNorthLabel);
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TargetRadarWindow::changeBackgroundMode);
    connect(m_setNorthButton, &QPushButton::clicked, this, &TargetRadarWindow::applyPendingNorthOffset);
    connect(m_resetNorthButton, &QPushButton::clicked, this, &TargetRadarWindow::resetNorthOffset);
    updateNorthLabel(m_radar->northOffsetDeg());

    m_targetCleanupTimer = new QTimer(this);
    m_targetCleanupTimer->setInterval(500);
    connect(m_targetCleanupTimer, &QTimer::timeout,
            this, &TargetRadarWindow::refreshExpiringTargets);
    m_targetCleanupTimer->start();
}

void TargetRadarWindow::setLivePanorama(const QImage &panorama, bool bwNeeds180Align)
{
    setLivePanoramas(QImage(), panorama, bwNeeds180Align);
}

void TargetRadarWindow::setLivePanoramas(const QImage &rgbPanorama,
                                         const QImage &bwPanorama,
                                         bool bwNeeds180Align)
{
    bool changed = false;
    if (!rgbPanorama.isNull() && rgbPanorama.cacheKey() != m_rgbPanorama.cacheKey()) {
        m_rgbPanorama = rgbPanorama;
        changed = true;
    }
    if (!bwPanorama.isNull() && bwPanorama.cacheKey() != m_bwPanorama.cacheKey()) {
        m_bwPanorama = bwPanorama;
        changed = true;
    }
    if (m_bwNeeds180Align != bwNeeds180Align) {
        m_bwNeeds180Align = bwNeeds180Align;
        changed = true;
    }
    if (!changed) return;

    m_fusedPanorama = QImage();
    applyBackgroundMode();
}

void TargetRadarWindow::setScanAngle(double angleDeg)
{
    m_radar->setScanAngle(angleDeg);
}

QString TargetRadarWindow::addOrUpdateTarget(const TargetRecord &target)
{
#ifndef DMX_ADVANCED_DETECTION
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        if (m_targets.at(i).time.isValid()
            && m_targets.at(i).time.msecsTo(QDateTime::fromMSecsSinceEpoch(nowMs)) > holdMs * 2) {
            m_targets.remove(i);
        }
    }

    int idx = -1;
    double bestCost = 1.0e9;
    for (int i = 0; i < m_targets.size(); ++i) {
        const double angleDiff = angleDistance(m_targets.at(i).azimuthDeg, target.azimuthDeg);
        const int yDiff = qAbs(m_targets.at(i).panoY - target.panoY);
        if (angleDiff <= 2.0 && yDiff <= targetVerticalTolerance()) {
            const double cost = angleDiff + (double)yDiff / (double)targetVerticalTolerance();
            if (cost >= bestCost) continue;
            bestCost = cost;
            idx = i;
        }
    }
    if (idx >= 0) {
        TargetRecord merged = target;
        merged.id = m_targets.at(idx).id;
        merged.firstTime = m_targets.at(idx).firstTime.isValid()
            ? m_targets.at(idx).firstTime
            : m_targets.at(idx).time;
        merged.lastTime = target.time;
        merged.hits = m_targets.at(idx).hits + 1;
        merged.confidence = qMax(m_targets.at(idx).confidence * 0.85, target.confidence);
        merged.score = qMax(m_targets.at(idx).score * 0.85, target.score);
        merged.trackAgeSec = merged.firstTime.isValid()
            ? merged.firstTime.msecsTo(target.time) / 1000.0
            : 0.0;
        merged.state = classifyTargetState(merged, nowMs);
        m_targets[idx] = merged;
    } else {
        TargetRecord fresh = target;
        fresh.firstTime = target.time;
        fresh.lastTime = target.time;
        fresh.hits = qMax(1, target.hits);
        fresh.state = classifyTargetState(fresh, nowMs);
        m_targets.push_front(fresh);
        idx = 0;
    }
    for (int i = 0; i < m_targets.size(); ++i) {
        m_targets[i].state = classifyTargetState(m_targets.at(i), nowMs);
    }
    while (m_targets.size() > 80) m_targets.removeLast();
    m_selected = idx;
    refreshTargetViews();
    return QString();
#else
    TargetRecord incoming = prepareAssociationFeatures(target);
    const ManualNegativeFeedbackResult feedback = m_manualNegativeFeedback.apply(
        &incoming, AppConfig::instance().fullWidth, AppConfig::instance().fullHeight);
    const QString feedbackDetail = feedback.weight < 0.999
        ? QStringLiteral(" feedback=%1 fp_matches=%2 dx=%3 dy=%4 hash=%5 raw=%6 adjusted=%7")
            .arg(feedback.weight, 0, 'f', 2)
            .arg(feedback.matchCount)
            .arg(feedback.bestDx)
            .arg(feedback.bestDy)
            .arg(feedback.bestHashDistance)
            .arg(feedback.originalScore, 0, 'f', 1)
            .arg(incoming.score, 0, 'f', 1)
        : QString();
    if (feedback.suppressed) {
        return QStringLiteral("manual_feedback_drop id=%1 class=%2 stream=%3%4")
            .arg(incoming.id,
                 incoming.className.isEmpty() ? QStringLiteral("unknown") : incoming.className,
                 incoming.stream,
                 feedbackDetail);
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        const QDateTime last = m_targets.at(i).lastTime.isValid()
            ? m_targets.at(i).lastTime
            : m_targets.at(i).time;
        if (last.isValid() && last.msecsTo(now) > targetAssociationRetentionMs()) {
            m_targets.remove(i);
            if (m_selected == i) m_selected = -1;
            else if (m_selected > i) --m_selected;
        }
    }

    int idx = -1;
    TargetMatch bestMatch;
    for (int i = 0; i < m_targets.size(); ++i) {
        const TargetMatch candidate = evaluateTargetMatch(m_targets.at(i), incoming);
        if (!candidate.eligible || candidate.cost >= bestMatch.cost) continue;
        bestMatch = candidate;
        idx = i;
    }

    QString association;
    if (idx >= 0) {
        const TargetRecord previous = m_targets.at(idx);
        TargetRecord merged = incoming;
        merged.id = previous.id;
        merged.firstTime = previous.firstTime.isValid() ? previous.firstTime : previous.time;
        merged.lastTime = incoming.time;
        merged.hits = previous.hits + 1;
        merged.stream = mergedStream(previous.stream, incoming.stream);
        merged.lastStream = incoming.lastStream;
        merged.confidence = qMax(previous.confidence * 0.92, incoming.confidence);
        merged.score = qMax(previous.score * 0.92, incoming.score);
        if (incoming.className.isEmpty()
            || (!previous.className.isEmpty()
                && previous.classConfidence > incoming.classConfidence)) {
            merged.className = previous.className;
            merged.classConfidence = previous.classConfidence;
        }
        merged.trackAgeSec = merged.firstTime.isValid()
            ? qMax<qint64>(0, merged.firstTime.msecsTo(incoming.time)) / 1000.0
            : 0.0;
        if (previous.imageConfidence > incoming.imageConfidence) {
            merged.imagePath = previous.imagePath;
            merged.hasRoiBox = previous.hasRoiBox;
            merged.roiBoxX1 = previous.roiBoxX1;
            merged.roiBoxY1 = previous.roiBoxY1;
            merged.roiBoxX2 = previous.roiBoxX2;
            merged.roiBoxY2 = previous.roiBoxY2;
            merged.boxAspect = previous.boxAspect;
            merged.boxArea = previous.boxArea;
            merged.imageConfidence = previous.imageConfidence;
            merged.appearanceHash = previous.appearanceHash;
            merged.hasAppearanceHash = previous.hasAppearanceHash;
        }
        merged.state = classifyTargetState(merged, nowMs);
        m_targets[idx] = merged;
        association = QStringLiteral(
            "merge id=%1 class=%2 mode=%3 dt=%4ms da=%5 dy=%6 shape=%7/%8 hash=%9 stream=%10 hits=%11")
            .arg(merged.id)
            .arg(merged.className.isEmpty() ? QStringLiteral("unknown") : merged.className)
            .arg(bestMatch.mode)
            .arg(bestMatch.dtMs)
            .arg(bestMatch.angleDiff, 0, 'f', 2)
            .arg(bestMatch.yDiff)
            .arg(bestMatch.areaRatio, 0, 'f', 2)
            .arg(bestMatch.aspectRatio, 0, 'f', 2)
            .arg(bestMatch.hashDistance)
            .arg(merged.stream)
            .arg(merged.hits)
            + feedbackDetail;
    } else {
        TargetRecord fresh = incoming;
        fresh.firstTime = incoming.time;
        fresh.lastTime = incoming.time;
        fresh.hits = qMax(1, incoming.hits);
        fresh.state = classifyTargetState(fresh, nowMs);
        m_targets.push_front(fresh);
        idx = 0;
        association = QStringLiteral("new id=%1 class=%2 stream=%3 frame=%4")
            .arg(fresh.id)
            .arg(fresh.className.isEmpty() ? QStringLiteral("unknown") : fresh.className)
            .arg(fresh.stream)
            .arg(fresh.frameKey)
            + feedbackDetail;
    }
    for (int i = 0; i < m_targets.size(); ++i) {
        m_targets[i].state = classifyTargetState(m_targets.at(i), nowMs);
    }
    while (m_targets.size() > 80) m_targets.removeLast();
    m_selected = idx;
    refreshTargetViews();
    return association;
#endif
}

void TargetRadarWindow::suppressStaticClutter(const QString &stream,
                                              const QString &className,
                                              int panoX,
                                              int panoY,
                                              int trackId,
                                              int stableHits,
                                              double contextScore)
{
#ifdef DMX_ADVANCED_DETECTION
    const QString normalizedSource = normalizedStream(stream);
    const QString normalizedClass = normalizedTargetClass(className);
    const int panoramaWidth = qMax(1, AppConfig::instance().fullWidth);
    int bestIndex = -1;
    int bestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < m_targets.size(); ++i) {
        const TargetRecord &target = m_targets.at(i);
        if (normalizedTargetClass(target.className) != normalizedClass) continue;
        const QString targetSource = normalizedStream(target.lastStream.isEmpty()
            ? target.stream
            : target.lastStream);
        if (targetSource != normalizedSource) continue;
        int dx = qAbs(target.panoX - panoX);
        dx = qMin(dx, panoramaWidth - dx);
        const int dy = qAbs(target.panoY - panoY);
        if (dx > 24 || dy > 12) continue;
        const int distance = dx + dy * 2;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    if (bestIndex < 0) return;

    TargetRecord &target = m_targets[bestIndex];
    target.staticSuppressed = true;
    target.staticTrackId = trackId;
    target.staticStableHits = stableHits;
    target.staticContextScore = contextScore;
    if (m_selected == bestIndex) {
        m_selected = -1;
        m_info->clearTarget();
        m_preview->setMessage(QStringLiteral("该候选已归入静态背景杂波。"));
    }
    refreshTargetViews();
#else
    Q_UNUSED(stream)
    Q_UNUSED(className)
    Q_UNUSED(panoX)
    Q_UNUSED(panoY)
    Q_UNUSED(trackId)
    Q_UNUSED(stableHits)
    Q_UNUSED(contextScore)
#endif
}

void TargetRadarWindow::clearTargets()
{
    m_targets.clear();
    m_selected = -1;
    refreshTargetViews();
    m_info->clearTarget();
    m_preview->setMessage(QStringLiteral("点击雷达目标点或方位扇区后显示对应图像。"));
}

void TargetRadarWindow::selectTarget(int index)
{
    if (index < 0 || index >= m_targets.size()) return;
    m_selected = index;
    applySelection();
}

void TargetRadarWindow::openTargetImage(int index)
{
    if (index < 0 || index >= m_targets.size()) return;
    m_selected = index;
    applySelection();

    const TargetRecord &target = m_targets.at(index);
    ImageViewerDialog *viewer = new ImageViewerDialog(this);
    const QString title = QStringLiteral("%1 目标图像").arg(target.id);
    viewer->setImagePath(target.imagePath, title);
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
}

#ifdef DMX_ADVANCED_DETECTION
void TargetRadarWindow::markFalsePositive(int sourceIndex, const QString &targetId)
{
    const auto resolveIndex = [this, sourceIndex, targetId]() {
        if (!targetId.isEmpty()) {
            for (int i = 0; i < m_targets.size(); ++i) {
                if (m_targets.at(i).id == targetId) return i;
            }
            return -1;
        }
        return sourceIndex >= 0 && sourceIndex < m_targets.size() ? sourceIndex : -1;
    };

    int index = resolveIndex();
    if (index < 0) {
        QMessageBox::information(this, QStringLiteral("目标已更新"),
                                 QStringLiteral("该目标已不在当前事件列表中，无需再次移除。"));
        return;
    }

    TargetRecord target = m_targets.at(index);
    const QString negativeRoot = ManualNegativeStore::defaultRootPath();
    const QString predictedClass = target.className.trimmed().isEmpty()
        ? QStringLiteral("未知目标")
        : target.className;
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        QStringLiteral("确认标记误检"),
        QStringLiteral("目标 %1（%2）将保存为训练负样本，并从当前列表和雷达中移除。\n\n保存目录：%3")
            .arg(target.id, predictedClass, negativeRoot),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    index = resolveIndex();
    if (index < 0) {
        QMessageBox::information(this, QStringLiteral("目标已更新"),
                                 QStringLiteral("确认期间该目标已离开当前事件列表，本次未保存。"));
        return;
    }
    target = m_targets.at(index);

    const ManualNegativeSaveResult saved = ManualNegativeStore::save(
        target, negativeRoot, qEnvironmentVariable("DMX_YOLO_MODEL"));
    if (!saved.success) {
        QMessageBox::warning(this, QStringLiteral("负样本保存失败"), saved.error);
        return;
    }

    QString feedbackError;
    if (!m_manualNegativeFeedback.reload(negativeRoot, &feedbackError)) {
        qWarning("manual negative feedback reload failed: %s", qPrintable(feedbackError));
    }

    m_targets.remove(index);
    if (m_selected > index) {
        --m_selected;
    } else if (m_selected == index) {
        m_selected = m_targets.isEmpty() ? -1 : qMin(index, m_targets.size() - 1);
    }

    if (m_targets.isEmpty()) {
        m_info->clearTarget();
        m_preview->setImagePath(QString());
    }
    refreshTargetViews();
    m_preview->setMessage(QStringLiteral("已标记误检并移除。负样本：%1").arg(saved.imagePath));
    qInfo("manual false positive saved: target=%s image=%s",
          qPrintable(target.id), qPrintable(saved.imagePath));
}
#endif

void TargetRadarWindow::handleAzimuth(double angleDeg)
{
    m_pendingNorthAzimuth = norm360Local(angleDeg);
    if (m_setNorthButton) m_setNorthButton->setEnabled(true);

    const double displayDeg = norm360Local(m_pendingNorthAzimuth - m_radar->northOffsetDeg());
    m_preview->setMessage(QStringLiteral("已选择方位：设备 %1 度，显示 %2 度。")
        .arg(m_pendingNorthAzimuth, 0, 'f', 1)
        .arg(displayDeg, 0, 'f', 1));
}

void TargetRadarWindow::changeBackgroundMode(int)
{
#ifdef DMX_ADVANCED_DETECTION
    applyBackgroundMode();
#else
    m_radar->setPanoramaBackground(m_bwPanorama, false);
#endif
}

void TargetRadarWindow::applyBackgroundMode()
{
    if (!m_radar) return;
    const int mode = m_mode ? m_mode->currentIndex() : 0;
    if (mode == 1 && !m_rgbPanorama.isNull()) {
        m_radar->setPanoramaBackground(m_rgbPanorama, false);
        return;
    }
    if (mode == 2 && !m_rgbPanorama.isNull() && !m_bwPanorama.isNull()) {
        if (m_fusedPanorama.isNull()) m_fusedPanorama = buildFusedPanorama();
        if (!m_fusedPanorama.isNull()) {
            m_radar->setPanoramaBackground(m_fusedPanorama, false);
            return;
        }
    }
    if (!m_bwPanorama.isNull()) {
        m_radar->setPanoramaBackground(m_bwPanorama, m_bwNeeds180Align);
    } else if (!m_rgbPanorama.isNull()) {
        m_radar->setPanoramaBackground(m_rgbPanorama, false);
    }
}

QImage TargetRadarWindow::buildFusedPanorama() const
{
    if (m_rgbPanorama.isNull() || m_bwPanorama.isNull()) return QImage();

    const QImage rgb = m_rgbPanorama.convertToFormat(QImage::Format_RGB32);
    QImage bw = m_bwNeeds180Align
        ? PolarPanoramaProjector::rotateAzimuth180(m_bwPanorama)
        : m_bwPanorama.convertToFormat(QImage::Format_RGB32);
    if (bw.size() != rgb.size()) {
        bw = bw.scaled(rgb.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else if (bw.format() != QImage::Format_RGB32) {
        bw = bw.convertToFormat(QImage::Format_RGB32);
    }

    QImage fused(rgb.size(), QImage::Format_RGB32);
    if (fused.isNull()) return QImage();
    for (int y = 0; y < rgb.height(); ++y) {
        const QRgb *rgbLine = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        const QRgb *bwLine = reinterpret_cast<const QRgb*>(bw.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb*>(fused.scanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb color = rgbLine[x];
            const int luminanceDelta = qGray(bwLine[x]) - qGray(color);
            const int detail = luminanceDelta * 55 / 100;
            dst[x] = qRgb(
                qBound(0, qRed(color) + detail, 255),
                qBound(0, qGreen(color) + detail, 255),
                qBound(0, qBlue(color) + detail, 255));
        }
    }
    return fused;
}

void TargetRadarWindow::applyPendingNorthOffset()
{
    if (m_pendingNorthAzimuth < 0.0) return;
    m_radar->setNorthOffsetDeg(m_pendingNorthAzimuth);
}

void TargetRadarWindow::resetNorthOffset()
{
    m_radar->setNorthOffsetDeg(0.0);
    updateNorthLabel(m_radar->northOffsetDeg());
}

void TargetRadarWindow::updateNorthLabel(double rawNorthDeg)
{
    const double raw = norm360Local(rawNorthDeg);
    if (m_northLabel) {
        m_northLabel->setText(QStringLiteral("显示正北：设备 %1°").arg(raw, 0, 'f', 1));
    }
    if (m_resetNorthButton) {
        m_resetNorthButton->setEnabled(angleDistance(raw, 0.0) > 0.05);
    }
}

void TargetRadarWindow::loadPanorama(const QString &path)
{
    m_bwPanorama = QImage(path);
    if (m_bwPanorama.isNull()) {
        m_preview->setMessage(QStringLiteral("全景图加载失败：%1").arg(path));
    }
    // The generated test asset is already BW-rotated by 180 degrees.
    m_bwNeeds180Align = false;
    applyBackgroundMode();
}

void TargetRadarWindow::buildMockTargets()
{
    const QStringList imgs = {
        QStringLiteral("/mnt/dmx4t/data/candidates/2026-06-26/BW/_65454_01736_001.jpg"),
        QStringLiteral("/mnt/dmx4t/data/candidates/2026-06-26/BW/_40284_02343_001.jpg"),
        QStringLiteral("/mnt/dmx4t/data/candidates/2026-06-26/BW/_38114_02208_001.jpg"),
        QStringLiteral("/mnt/dmx4t/data/candidates/2026-06-26/BW/_43919_02058_001.jpg")
    };
    for (int i = 0; i < imgs.size(); ++i) {
        TargetRecord t;
        t.id = QStringLiteral("T-%1").arg(24 + i, 3, 10, QLatin1Char('0'));
        t.time = QDateTime(QDate(2026, 6, 26), QTime(11, 58 + i, 16 + i * 7));
        t.firstTime = t.time.addSecs(-2 - i);
        t.lastTime = t.time;
        t.imagePath = imgs.at(i);
        t.stream = QStringLiteral("BW");
        t.className = (i == 1) ? QStringLiteral("bird") : QStringLiteral("drone");
        t.state = (i == 0) ? QStringLiteral("new") : QStringLiteral("tracking");
        t.azimuthDeg = norm360Local(34.2 + i * 84.0);
        t.confidence = 0.88 - i * 0.06;
        t.classConfidence = t.confidence;
        t.score = 86 - i * 7;
        t.trackAgeSec = 1.8 + i * 0.9;
        t.hits = 1 + i;
        t.panoX = 6224 + i * 5120;
        t.panoY = 1736 + i * 80;
        t.frameX = 1736 + i * 48;
        t.frameY = 812 + i * 24;
        m_targets.push_back(t);
    }
}


void TargetRadarWindow::refreshTargetViews()
{
    QVector<int> indexes = visibleTargetIndexes();
    QVector<TargetRecord> visible;
    QStringList visibleIds;
    visible.reserve(indexes.size());
    visibleIds.reserve(indexes.size());
    for (int idx : indexes) {
        if (idx < 0 || idx >= m_targets.size()) continue;
        visible.push_back(m_targets.at(idx));
        visibleIds.push_back(m_targets.at(idx).id);
    }
    m_visibleTargetIds = visibleIds;
    m_list->setTargets(visible, indexes);
    m_radar->setTargets(m_targets);
    emit visibleTargetsChanged(visible);
    applySelection();
}

void TargetRadarWindow::refreshExpiringTargets()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    bool changed = false;

#ifdef DMX_ADVANCED_DETECTION
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        const QDateTime last = m_targets.at(i).lastTime.isValid()
            ? m_targets.at(i).lastTime
            : m_targets.at(i).time;
        if (!last.isValid() || last.msecsTo(now) <= targetAssociationRetentionMs()) continue;
        m_targets.remove(i);
        if (m_selected == i) m_selected = -1;
        else if (m_selected > i) --m_selected;
        changed = true;
    }
#endif

    for (int i = 0; i < m_targets.size(); ++i) {
        const QString state = classifyTargetState(m_targets.at(i), nowMs);
        if (m_targets.at(i).state == state) continue;
        m_targets[i].state = state;
        changed = true;
    }

    const QVector<int> indexes = visibleTargetIndexes();
    QStringList visibleIds;
    visibleIds.reserve(indexes.size());
    for (int idx : indexes) {
        if (idx >= 0 && idx < m_targets.size()) visibleIds.push_back(m_targets.at(idx).id);
    }
    if (visibleIds != m_visibleTargetIds) changed = true;

#ifdef DMX_ADVANCED_DETECTION
    if (m_selected >= 0 && m_selected < m_targets.size()) {
        const TargetRecord &selected = m_targets.at(m_selected);
        const QDateTime last = selected.lastTime.isValid() ? selected.lastTime : selected.time;
        if (selected.staticSuppressed
            || (last.isValid() && last.msecsTo(now) > targetVisibleRetentionMs())) {
            m_selected = -1;
            changed = true;
        }
    }
#endif

    if (changed) refreshTargetViews();
}

QVector<int> TargetRadarWindow::visibleTargetIndexes() const
{
    QVector<int> indexes;
    indexes.reserve(m_targets.size());
#ifndef DMX_ADVANCED_DETECTION
    for (int i = 0; i < m_targets.size(); ++i) indexes.push_back(i);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
#else
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets.at(i).staticSuppressed) continue;
        const QDateTime last = m_targets.at(i).lastTime.isValid()
            ? m_targets.at(i).lastTime
            : m_targets.at(i).time;
        if (!last.isValid() || last.msecsTo(now) <= targetVisibleRetentionMs()) {
            indexes.push_back(i);
        }
    }
#endif

    std::sort(indexes.begin(), indexes.end(), [this, nowMs](int a, int b) {
        return targetDisplayPriority(m_targets.at(a), nowMs) > targetDisplayPriority(m_targets.at(b), nowMs);
    });

    if (indexes.size() > m_maxListTargets) indexes.resize(m_maxListTargets);
#ifndef DMX_ADVANCED_DETECTION
    if (m_selected >= 0 && m_selected < m_targets.size() && !indexes.contains(m_selected)) {
#else
    if (m_selected >= 0 && m_selected < m_targets.size()
        && !m_targets.at(m_selected).staticSuppressed
        && !indexes.contains(m_selected)
        && targetVisibleRetentionMs() >= (m_targets.at(m_selected).lastTime.isValid()
            ? m_targets.at(m_selected).lastTime
            : m_targets.at(m_selected).time).msecsTo(QDateTime::currentDateTime())) {
#endif
        if (indexes.size() < m_maxListTargets) indexes.push_back(m_selected);
        else if (!indexes.isEmpty()) indexes[indexes.size() - 1] = m_selected;
    }
    return indexes;
}

double TargetRadarWindow::targetDisplayPriority(const TargetRecord &target, qint64 nowMs) const
{
    const int holdMs = qMax(1000, AppConfig::instance().detectRadarHoldMs);
    const QDateTime last = target.lastTime.isValid() ? target.lastTime : target.time;
    const qint64 ageMs = last.isValid()
        ? qMax<qint64>(0, last.msecsTo(QDateTime::fromMSecsSinceEpoch(nowMs)))
        : holdMs;
    const double freshness = qBound(0.0, 1.0 - (double)ageMs / (double)holdMs, 1.0);
    const double confidence = qBound(0.0, target.confidence, 1.0);

    double stateWeight = 80.0;
    if (target.state.compare(QStringLiteral("confirmed"), Qt::CaseInsensitive) == 0) stateWeight = 320.0;
    else if (target.state.compare(QStringLiteral("tracking"), Qt::CaseInsensitive) == 0) stateWeight = 220.0;
    else if (target.state.compare(QStringLiteral("stale"), Qt::CaseInsensitive) == 0) stateWeight = -80.0;

    return stateWeight
        + confidence * 180.0
        + freshness * 120.0
        + qMin(5, qMax(1, target.hits)) * 32.0
        + qMin(120.0, qMax(0.0, target.score) / 4.0);
}

void TargetRadarWindow::applySelection()
{
    if (m_selected < 0 || m_selected >= m_targets.size()) {
        m_list->setSelectedTarget(-1);
        m_radar->setSelectedTarget(-1);
        m_info->clearTarget();
        m_preview->setImagePath(QString());
        return;
    }
    m_list->setSelectedTarget(m_selected);
    m_radar->setSelectedTarget(m_selected);
    m_info->setTarget(m_targets.at(m_selected));
    m_preview->setImagePath(m_targets.at(m_selected).imagePath);
}
