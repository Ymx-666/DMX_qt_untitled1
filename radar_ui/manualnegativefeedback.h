#ifndef MANUALNEGATIVEFEEDBACK_H
#define MANUALNEGATIVEFEEDBACK_H

#include "targetrecord.h"

#include <QDateTime>
#include <QString>
#include <QVector>

struct ManualNegativeFeedbackResult
{
    bool suppressed = false;
    double weight = 1.0;
    double originalConfidence = 0.0;
    double originalScore = 0.0;
    int matchCount = 0;
    int bestDx = -1;
    int bestDy = -1;
    int bestHashDistance = -1;
};

class ManualNegativeFeedback
{
public:
    bool reload(const QString &rootPath, QString *error = nullptr);
    ManualNegativeFeedbackResult apply(TargetRecord *target,
                                       int panoramaWidth,
                                       int panoramaHeight) const;
    int sampleCount() const;

private:
    struct Entry
    {
        QDateTime markedAt;
        QString className;
        QString stream;
        int panoX = 0;
        int panoY = 0;
        quint64 appearanceHash = 0;
        bool hasAppearanceHash = false;
    };

    QVector<Entry> m_entries;
};

#endif // MANUALNEGATIVEFEEDBACK_H
