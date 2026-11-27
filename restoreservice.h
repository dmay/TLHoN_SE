#ifndef RESTORESERVICE_H
#define RESTORESERVICE_H

#include "backupservice.h"

// A preview binds confirmation to both verified snapshot bytes and the actual
// current pair. Missing files have no map entry; an empty file has an entry.
struct RestorePreview
{
    BackupSnapshot snapshot;
    QMap<QString, QByteArray> before;
    QMap<QString, QByteArray> after;
    QString targetDirectory;
    bool primaryNeedsRecovery = false;
};

class RestoreService
{
public:
    using GameCheck = std::function<bool(QString &)>;
    // Deterministic failure injection before each changed file is published.
    // Production callers leave this empty.
    using StepCheck = std::function<bool(int, QString &)>;

    explicit RestoreService(SavePaths paths, const QString &storageDirectory = {},
                            GameCheck gameCheck = {}, StepCheck stepCheck = {});
    bool preview(const QString &id, RestorePreview &result, QString &error) const;
    bool restore(const RestorePreview &confirmed, BackupSnapshot &safety, QString &error) const;

    static QString journalPath(const SavePaths &paths);
    static bool hasPending(const SavePaths &paths);
    // Does nothing if no journal exists. Otherwise restores the exact old pair
    // only if all current files match a known transaction state.
    static bool recoverPending(const SavePaths &paths, QString &error, GameCheck gameCheck = {});
    static bool gameClosed(QString &error);

private:
    SavePaths paths;
    BackupService backups;
    GameCheck checkGame;
    StepCheck checkStep;
};

#endif
