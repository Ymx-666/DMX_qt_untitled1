#ifndef MANUALNEGATIVESTORE_H
#define MANUALNEGATIVESTORE_H

#include "targetrecord.h"

#include <QString>

struct ManualNegativeSaveResult
{
    bool success = false;
    QString rootPath;
    QString imagePath;
    QString labelPath;
    QString manifestPath;
    QString error;
};

class ManualNegativeStore
{
public:
    static QString defaultRootPath();
    static ManualNegativeSaveResult save(const TargetRecord &target,
                                         const QString &rootPath = QString(),
                                         const QString &modelPath = QString());
};

#endif // MANUALNEGATIVESTORE_H
