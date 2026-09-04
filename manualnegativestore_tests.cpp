#include "radar_ui/manualnegativestore.h"
#include "radar_ui/manualnegativefeedback.h"

#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class ManualNegativeStoreTests : public QObject
{
    Q_OBJECT

private slots:
    void savesImageEmptyLabelAndMetadata();
    void rejectsMissingSourceImage();
    void repeatedNearbyMarksWeakenOrdinaryCandidate();
    void strongCandidateIsOnlyDownWeighted();
    void distantCandidateIsUnaffected();
};

void ManualNegativeStoreTests::savesImageEmptyLabelAndMetadata()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString sourcePath = temp.filePath(QStringLiteral("source.png"));
    QImage source(64, 48, QImage::Format_RGB32);
    source.fill(QColor(30, 140, 60));
    QVERIFY(source.save(sourcePath));

    TargetRecord target;
    target.id = QStringLiteral("T-042");
    target.time = QDateTime::currentDateTime();
    target.firstTime = target.time.addSecs(-8);
    target.lastTime = target.time;
    target.imagePath = sourcePath;
    target.stream = QStringLiteral("RGB+BW");
    target.lastStream = QStringLiteral("BW");
    target.className = QStringLiteral("drone");
    target.state = QStringLiteral("confirmed");
    target.confidence = 0.73;
    target.classConfidence = 0.71;
    target.score = 82.0;
    target.hits = 12;
    target.azimuthDeg = 123.4;
    target.panoX = 1024;
    target.panoY = 88;
    target.frameKey = QStringLiteral("BW_frame_001");
    target.hasRoiBox = true;
    target.roiBoxX1 = 10;
    target.roiBoxY1 = 11;
    target.roiBoxX2 = 30;
    target.roiBoxY2 = 31;

    const QString root = temp.filePath(QStringLiteral("manual_negatives"));
    const ManualNegativeSaveResult saved = ManualNegativeStore::save(
        target, root, QStringLiteral("/models/weights5/best.onnx"));
    QVERIFY2(saved.success, qPrintable(saved.error));
    QVERIFY(QFileInfo::exists(saved.imagePath));
    QCOMPARE(QImage(saved.imagePath).size(), source.size());
    QCOMPARE(QFileInfo(saved.labelPath).size(), qint64(0));
    QVERIFY(QFileInfo::exists(QDir(root).filePath(QStringLiteral("README.md"))));

    QFile manifest(saved.manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readLine().trimmed());
    QVERIFY(document.isObject());
    const QJsonObject metadata = document.object();
    QCOMPARE(metadata.value(QStringLiteral("sample_type")).toString(),
             QStringLiteral("manual_false_positive"));
    QCOMPARE(metadata.value(QStringLiteral("target_id")).toString(), target.id);
    QCOMPARE(metadata.value(QStringLiteral("predicted_class")).toString(), target.className);
    QCOMPARE(metadata.value(QStringLiteral("model_path")).toString(),
             QStringLiteral("/models/weights5/best.onnx"));
    QCOMPARE(metadata.value(QStringLiteral("predicted_box")).toArray().size(), 4);

    const ManualNegativeSaveResult second = ManualNegativeStore::save(target, root);
    QVERIFY2(second.success, qPrintable(second.error));
    QVERIFY(second.imagePath != saved.imagePath);
}

void ManualNegativeStoreTests::rejectsMissingSourceImage()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TargetRecord target;
    target.imagePath = temp.filePath(QStringLiteral("missing.jpg"));

    const ManualNegativeSaveResult saved = ManualNegativeStore::save(
        target, temp.filePath(QStringLiteral("manual_negatives")));
    QVERIFY(!saved.success);
    QVERIFY(!saved.error.isEmpty());
}

static TargetRecord feedbackTarget(const QString &imagePath,
                                   int panoX,
                                   int panoY,
                                   double confidence)
{
    TargetRecord target;
    target.id = QStringLiteral("T-feedback");
    target.time = QDateTime::currentDateTime();
    target.imagePath = imagePath;
    target.stream = QStringLiteral("BW");
    target.lastStream = QStringLiteral("BW");
    target.className = QStringLiteral("drone");
    target.confidence = confidence;
    target.classConfidence = confidence;
    target.imageConfidence = confidence;
    target.score = confidence * 255.0;
    target.panoX = panoX;
    target.panoY = panoY;
    target.hasRoiBox = true;
    target.roiBoxX1 = 16;
    target.roiBoxY1 = 12;
    target.roiBoxX2 = 48;
    target.roiBoxY2 = 38;
    target.hasAppearanceHash = true;
    target.appearanceHash = Q_UINT64_C(0x123456789abcdef0);
    return target;
}

static QString createFeedbackManifest(QTemporaryDir &temp)
{
    const QString sourcePath = temp.filePath(QStringLiteral("feedback_source.png"));
    QImage source(64, 48, QImage::Format_RGB32);
    source.fill(QColor(80, 130, 40));
    if (!source.save(sourcePath)) return QString();

    const QString root = temp.filePath(QStringLiteral("feedback"));
    const TargetRecord marked = feedbackTarget(sourcePath, 20000, 500, 0.45);
    for (int i = 0; i < 3; ++i) {
        if (!ManualNegativeStore::save(marked, root).success) return QString();
    }
    return root;
}

void ManualNegativeStoreTests::repeatedNearbyMarksWeakenOrdinaryCandidate()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = createFeedbackManifest(temp);
    QVERIFY(!root.isEmpty());

    ManualNegativeFeedback feedback;
    QString error;
    QVERIFY2(feedback.reload(root, &error), qPrintable(error));
    QCOMPARE(feedback.sampleCount(), 3);

    TargetRecord candidate = feedbackTarget(
        temp.filePath(QStringLiteral("feedback_source.png")), 20120, 540, 0.45);
    const ManualNegativeFeedbackResult result = feedback.apply(&candidate, 65536, 4096);
    QVERIFY(result.matchCount >= 3);
    QVERIFY(result.weight < 0.4);
    QVERIFY(candidate.score < result.originalScore);
    QVERIFY(result.suppressed);
}

void ManualNegativeStoreTests::strongCandidateIsOnlyDownWeighted()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = createFeedbackManifest(temp);
    QVERIFY(!root.isEmpty());

    ManualNegativeFeedback feedback;
    QVERIFY(feedback.reload(root));
    TargetRecord candidate = feedbackTarget(
        temp.filePath(QStringLiteral("feedback_source.png")), 20020, 510, 0.85);
    const ManualNegativeFeedbackResult result = feedback.apply(&candidate, 65536, 4096);
    QVERIFY(result.weight < 1.0);
    QVERIFY(!result.suppressed);
    QVERIFY(candidate.confidence < 0.85);
}

void ManualNegativeStoreTests::distantCandidateIsUnaffected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = createFeedbackManifest(temp);
    QVERIFY(!root.isEmpty());

    ManualNegativeFeedback feedback;
    QVERIFY(feedback.reload(root));
    TargetRecord candidate = feedbackTarget(
        temp.filePath(QStringLiteral("feedback_source.png")), 30000, 2000, 0.45);
    const ManualNegativeFeedbackResult result = feedback.apply(&candidate, 65536, 4096);
    QCOMPARE(result.weight, 1.0);
    QVERIFY(!result.suppressed);
    QCOMPARE(candidate.confidence, 0.45);
}

QTEST_APPLESS_MAIN(ManualNegativeStoreTests)

#include "manualnegativestore_tests.moc"
