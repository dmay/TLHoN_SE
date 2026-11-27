#ifndef BACKUPSERVICE_H
#define BACKUPSERVICE_H

#include "savepaths.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <functional>

class QSettings;

enum class BackupKind { Normal, Legacy, Safety };
enum class BackupFileState { Present, Missing, NotCaptured };

struct BackupFile
{
    QString name;
    BackupFileState state = BackupFileState::Missing;
    qint64 size = 0;
    QByteArray sha256; // lowercase hex
};

struct BackupSnapshot
{
    QString id; // directory basename, not an arbitrary path
    QDateTime createdAt;
    QString label;
    QString sourceDirectory;
    QString sourceProfile;
    BackupKind kind = BackupKind::Normal;
    QList<BackupFile> files;
    bool valid = false; // manifest and disk contents verified, not a save parse
    QString error;
};

// Byte-exact, standalone storage. Does not parse, restore or modify game saves.
// For restore, use readVerified() and validate the returned primary with
// SaveDocument before any destructive action.
class BackupService
{
public:
    using CancelCheck = std::function<bool()>;

    explicit BackupService(SavePaths paths, const QString &storageDirectory = {});
    const QString &storageDirectory() const { return root; }
    static QString defaultStorageDirectory();
    static QString configuredStorageDirectory(const QSettings &settings);
    // Validates/creates a separate storage directory before persisting it.
    static bool configureStorageDirectory(QSettings &settings, const SavePaths &paths,
                                          const QString &directory, QString &error);

    bool createSnapshot(const QString &label, BackupSnapshot &result, QString &error,
                        const CancelCheck &canceled = {}) const;
    // Explicit import; never moves, overwrites or deletes the original _bckp.
    bool importLegacy(const QString &label, BackupSnapshot &result, QString &error,
                      const CancelCheck &canceled = {}) const;

    // Returns newest first, including invalid/incomplete managed entries with
    // their errors. A root-level access error is returned separately.
    bool list(QList<BackupSnapshot> &result, QString &error) const;
    bool inspect(const QString &id, BackupSnapshot &result, QString &error) const;
    bool readVerified(const QString &id, BackupSnapshot &result,
                      QMap<QString, QByteArray> &bytes, QString &error) const;
    bool renameLabel(const QString &id, const QString &label, QString &error) const;
    // Caller must obtain explicit confirmation for the selected ID.
    bool deleteSnapshot(const QString &id, bool confirmed, QString &error) const;

private:
    friend class RestoreService;
    bool ensureRoot(bool create, QString &error) const;
    // Safety capture is only called while RestoreService holds the source lock.
    bool capture(BackupKind kind, const QString &label, BackupSnapshot &result,
                 QString &error, const CancelCheck &canceled, bool sourceLocked = false) const;
    bool snapshotPath(const QString &id, QString &path, QString &error) const;
    SavePaths paths;
    QString root;
};

#endif
