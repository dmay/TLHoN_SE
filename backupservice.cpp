#include "backupservice.h"
#include "restoreservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace {
const QString primaryName = "SaveData.txt";
const QString gameBackupName = "SaveData.txt.bak";
const QString legacyName = "SaveData.txt_bckp";
const QString manifestName = "manifest.json";
const QString settingsKey = "Backup/storageDirectory";
const QString stampFormat = "yyyyMMdd'T'HHmmsszzz'Z'";

bool fail(QString &error, const QString &message)
{
    error = message;
    return false;
}

bool linked(const QFileInfo &info)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
    return info.isSymLink() || info.isJunction();
#else
    return info.isSymLink();
#endif
}

// Resolve even a not-yet-created root through its nearest existing ancestor,
// so a symlink/junction alias cannot put storage inside the game folder.
QString resolvedPath(const QString &path)
{
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (info.exists())
        return QDir::cleanPath(info.canonicalFilePath());
    const QString parent = info.absolutePath();
    if (parent == info.absoluteFilePath() || linked(info))
        return {};
    const auto resolvedParent = resolvedPath(parent);
    if (resolvedParent.isEmpty())
        return {};
    return QDir::cleanPath(QDir(resolvedParent).filePath(info.fileName()));
}

bool within(const QString &path, const QString &parent)
{
#ifdef Q_OS_WIN
    constexpr auto cs = Qt::CaseInsensitive;
#else
    constexpr auto cs = Qt::CaseSensitive;
#endif
    const auto prefix = parent.endsWith('/') ? parent : parent + '/';
    return path.compare(parent, cs) == 0 || path.startsWith(prefix, cs);
}

bool overlaps(const QString &a, const QString &b)
{
    return within(a, b) || within(b, a);
}

bool validId(const QString &id)
{
    static const QRegularExpression pattern(
        "\\A[0-9]{8}T[0-9]{9}Z-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
    return pattern.match(id).hasMatch();
}

QString newId(const QDateTime &time)
{
    return time.toString(stampFormat) + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool readRegular(const QString &path, QByteArray &bytes, QString &error)
{
    const QFileInfo before(path);
    if (linked(before) || !before.isFile())
        return fail(error, QString("Not a regular file: %1").arg(path));
    const auto size = before.size();
    const auto modified = before.lastModified();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QString("Cannot open %1: %2").arg(path, file.errorString()));
    bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(error, QString("Cannot read %1: %2").arg(path, file.errorString()));
    const QFileInfo after(path);
    if (linked(after) || !after.isFile() || after.size() != size ||
        bytes.size() != size || after.lastModified() != modified)
        return fail(error, QString("File changed while reading: %1").arg(path));
    return true;
}

bool writeAtomic(const QString &path, const QByteArray &bytes, QString &error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QString("Cannot stage %1: %2").arg(path, file.errorString()));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, QString("Cannot write %1: %2").arg(path, file.errorString()));
    }
    if (!file.commit())
        return fail(error, QString("Cannot commit %1: %2").arg(path, file.errorString()));
    return true;
}

bool present(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || linked(info);
}

QString stateName(BackupFileState state)
{
    switch (state) {
    case BackupFileState::Present: return "present";
    case BackupFileState::Missing: return "missing";
    case BackupFileState::NotCaptured: return "notCaptured";
    }
    return {};
}

QByteArray encodeManifest(const BackupSnapshot &snapshot)
{
    QJsonArray files;
    for (const auto &file : snapshot.files) {
        QJsonObject entry{{"name", file.name}, {"state", stateName(file.state)}};
        if (file.state == BackupFileState::Present) {
            entry["size"] = QString::number(file.size); // exact 64-bit decimal
            entry["sha256"] = QString::fromLatin1(file.sha256);
        }
        files.append(entry);
    }
    const QJsonObject manifest{
        {"version", 1}, {"id", snapshot.id},
        {"kind", snapshot.kind == BackupKind::Legacy ? "legacy" :
                 snapshot.kind == BackupKind::Safety ? "safety" : "normal"},
        {"createdAt", snapshot.createdAt.toString(Qt::ISODateWithMs)},
        {"label", snapshot.label}, {"sourceDirectory", snapshot.sourceDirectory},
        {"sourceProfile", snapshot.sourceProfile}, {"files", files}};
    return QJsonDocument(manifest).toJson(QJsonDocument::Indented);
}

bool decodeManifest(const QByteArray &bytes, const QString &id, BackupSnapshot &snapshot, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, "Malformed snapshot manifest.");
    const auto m = document.object();
    if (m.value("version") != QJsonValue(1) || m.value("id").toString() != id ||
        !m.value("createdAt").isString() || !m.value("label").isString() ||
        !m.value("sourceDirectory").isString() || m.value("sourceDirectory").toString().isEmpty() ||
        !m.value("sourceProfile").isString() || m.value("sourceProfile").toString().isEmpty() ||
        !m.value("files").isArray())
        return fail(error, "Invalid or unsupported snapshot manifest.");
    const auto timestamp = m.value("createdAt").toString();
    const auto created = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!created.isValid() || !timestamp.endsWith('Z') ||
        !id.startsWith(created.toUTC().toString(stampFormat) + "-"))
        return fail(error, "Invalid snapshot timestamp.");
    const auto kind = m.value("kind").toString();
    if (kind != "normal" && kind != "legacy" && kind != "safety")
        return fail(error, "Invalid snapshot kind.");

    snapshot.createdAt = created.toUTC();
    snapshot.kind = kind == "legacy" ? BackupKind::Legacy : kind == "safety" ? BackupKind::Safety : BackupKind::Normal;
    snapshot.label = m.value("label").toString();
    snapshot.sourceDirectory = m.value("sourceDirectory").toString();
    snapshot.sourceProfile = m.value("sourceProfile").toString();

    const auto files = m.value("files").toArray();
    const QString first = snapshot.kind == BackupKind::Legacy ? legacyName : primaryName;
    if (files.size() != 2)
        return fail(error, "Snapshot must describe exactly two file states.");
    for (int i = 0; i < files.size(); ++i) {
        if (!files[i].isObject())
            return fail(error, "Invalid snapshot file entry.");
        const auto entry = files[i].toObject();
        BackupFile file;
        file.name = entry.value("name").toString();
        const auto state = entry.value("state").toString();
        if (file.name != (i == 0 ? first : gameBackupName))
            return fail(error, "Unexpected snapshot filename.");
        if (state == "present") file.state = BackupFileState::Present;
        else if (state == "missing") file.state = BackupFileState::Missing;
        else if (state == "notCaptured") file.state = BackupFileState::NotCaptured;
        else return fail(error, "Invalid snapshot file state.");
        if ((i == 0 && (file.state == BackupFileState::NotCaptured ||
                         (snapshot.kind != BackupKind::Safety && file.state != BackupFileState::Present))) ||
            (i == 1 && snapshot.kind == BackupKind::Legacy && file.state != BackupFileState::NotCaptured) ||
            (i == 1 && snapshot.kind != BackupKind::Legacy && file.state == BackupFileState::NotCaptured))
            return fail(error, "Invalid file state for snapshot kind.");
        if (file.state == BackupFileState::Present) {
            bool ok = false;
            const auto size = entry.value("size").toString();
            file.size = size.toLongLong(&ok);
            const auto sha = entry.value("sha256").toString();
            static const QRegularExpression digest("\\A[0-9a-f]{64}\\z");
            if (!ok || file.size < 0 || QString::number(file.size) != size || !digest.match(sha).hasMatch())
                return fail(error, "Invalid snapshot file size or checksum.");
            file.sha256 = sha.toLatin1();
        } else if (entry.contains("size") || entry.contains("sha256")) {
            return fail(error, "Unexpected data for an uncaptured file.");
        }
        snapshot.files.append(file);
    }
    return true;
}

// Only fixed, regular files are ever handled. Refuse unexpected contents,
// including links, nested directories and files not belonging to this format.
bool safeContents(const QString &path, const QStringList &allowed, QString &error)
{
    const QDir dir(path);
    if (!QFileInfo(path).isReadable())
        return fail(error, QString("Cannot inspect snapshot directory: %1").arg(path));
    for (const auto &entry : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (!allowed.contains(entry.fileName()) || linked(entry) || !entry.isFile())
            return fail(error, QString("Unexpected or unsafe snapshot entry: %1").arg(entry.filePath()));
    }
    return true;
}

bool verifyDirectory(const QString &path, const QString &id, BackupSnapshot &snapshot,
                     QMap<QString, QByteArray> &bytes, QString &error)
{
    snapshot = {};
    snapshot.id = id;
    bytes.clear();
    const auto reject = [&]() {
        snapshot.error = error;
        bytes.clear();
        return false;
    };
    QByteArray manifest;
    const QDir dir(path);
    if (!readRegular(dir.filePath(manifestName), manifest, error) ||
        !decodeManifest(manifest, id, snapshot, error))
        return reject();
    QStringList allowed{manifestName};
    for (const auto &record : snapshot.files) {
        const auto filePath = dir.filePath(record.name);
        if (record.state != BackupFileState::Present) {
            if (present(filePath)) {
                error = QString("Snapshot contains an uncaptured file: %1").arg(record.name);
                return reject();
            }
            continue;
        }
        allowed.append(record.name);
        QByteArray content;
        if (!readRegular(filePath, content, error))
            return reject();
        if (content.size() != record.size || hash(content) != record.sha256) {
            error = QString("Snapshot size or checksum mismatch: %1").arg(record.name);
            return reject();
        }
        bytes.insert(record.name, content);
    }
    if (!safeContents(path, allowed, error))
        return reject();
    snapshot.valid = true;
    error.clear();
    return true;
}

bool readSource(const SavePaths &paths, BackupKind kind, QMap<QString, QByteArray> &bytes, QString &error)
{
    bytes.clear();
    const auto path = kind == BackupKind::Legacy ? paths.legacyEditorBackup() : paths.primary();
    QByteArray content;
    if (kind != BackupKind::Safety || present(path)) {
        if (!readRegular(path, content, error))
            return false;
        bytes.insert(kind == BackupKind::Legacy ? legacyName : primaryName, content);
    }
    if (kind != BackupKind::Legacy && present(paths.gameBackup())) {
        if (!readRegular(paths.gameBackup(), content, error))
            return false;
        bytes.insert(gameBackupName, content);
    }
    return true;
}
}

BackupService::BackupService(SavePaths paths, const QString &storageDirectory)
    : paths(std::move(paths)),
      root(QDir(storageDirectory.isEmpty() ? defaultStorageDirectory() : storageDirectory).absolutePath())
{}

QString BackupService::defaultStorageDirectory()
{
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    return QDir(local.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) : local)
        .filePath("TLHoN_SE/Backups");
}

QString BackupService::configuredStorageDirectory(const QSettings &settings)
{
    const auto value = settings.value(settingsKey).toString();
    return value.isEmpty() ? defaultStorageDirectory() : value;
}

bool BackupService::configureStorageDirectory(QSettings &settings, const SavePaths &paths,
                                              const QString &directory, QString &error)
{
    BackupService service(paths, directory);
    if (!service.ensureRoot(true, error))
        return false;
    if (!QFileInfo(service.root).isWritable())
        return fail(error, QString("Backup storage is not writable: %1").arg(service.root));
    settings.setValue(settingsKey, service.root);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(error, "Could not persist the backup storage preference.");
    error.clear();
    return true;
}

bool BackupService::ensureRoot(bool create, QString &error) const
{
    const QFileInfo info(root);
    const auto resolved = resolvedPath(root);
    const auto source = resolvedPath(paths.directory());
    const auto defaultSource = resolvedPath(SavePaths().directory());
    if (resolved.isEmpty() || source.isEmpty() || defaultSource.isEmpty() ||
        overlaps(resolved, source) || overlaps(resolved, defaultSource))
        return fail(error, "Backup storage must be separate from the game save directory.");
    if (linked(info) || (info.exists() && !info.isDir()))
        return fail(error, QString("Invalid backup storage directory: %1").arg(root));
    if (create && !QDir().mkpath(root))
        return fail(error, QString("Cannot create backup storage: %1").arg(root));
    const QFileInfo current(root);
    if (linked(current) || (current.exists() && (!current.isDir() || !current.isReadable())))
        return fail(error, QString("Cannot access backup storage: %1").arg(root));
    error.clear();
    return true;
}

bool BackupService::snapshotPath(const QString &id, QString &path, QString &error) const
{
    if (!validId(id))
        return fail(error, "Invalid snapshot ID (expected managed directory name).");
    if (!ensureRoot(false, error))
        return false;
    path = QDir(root).filePath(id);
    const QFileInfo info(path);
    if (linked(info) || !info.isDir())
        return fail(error, QString("Missing or unsafe snapshot directory: %1").arg(id));
    if (!within(info.canonicalFilePath(), resolvedPath(root)))
        return fail(error, "Snapshot directory is outside backup storage.");
    return true;
}

bool BackupService::capture(BackupKind kind, const QString &label, BackupSnapshot &result,
                             QString &error, const CancelCheck &canceled, bool sourceLocked) const
{
    result = {};
    if (canceled && canceled())
        return fail(error, "Backup canceled.");
    if (!ensureRoot(true, error))
        return false;
    QLockFile catalogLock(QDir(root).filePath(".catalog.lock"));
    if (!catalogLock.tryLock(0))
        return fail(error, "Cannot lock backup storage. Check permissions and other editor instances.");
    // Cooperate with SaveStore. The game/Steam do not honor the editor lock;
    // compare the complete pair again just before publishing the snapshot.
    QLockFile sourceLock(paths.primary() + ".editor.lock");
    sourceLock.setStaleLockTime(0);
    if (!sourceLocked && !sourceLock.tryLock(0))
        return fail(error, "Cannot lock save directory. Close the game and other editor instances, and check permissions.");
    if (RestoreService::hasPending(paths))
        return fail(error, "Recover the interrupted restore before creating another snapshot.");
    QMap<QString, QByteArray> captured;
    if (!readSource(paths, kind, captured, error))
        return false;

    BackupSnapshot snapshot;
    snapshot.createdAt = QDateTime::currentDateTimeUtc();
    snapshot.id = newId(snapshot.createdAt);
    snapshot.kind = kind;
    snapshot.label = label;
    snapshot.sourceDirectory = paths.directory();
    snapshot.sourceProfile = qEnvironmentVariable("USERPROFILE", QDir::homePath());
    for (const auto &name : {kind == BackupKind::Legacy ? legacyName : primaryName, gameBackupName}) {
        BackupFile record;
        record.name = name;
        if (captured.contains(name)) {
            record.state = BackupFileState::Present;
            record.size = captured.value(name).size();
            record.sha256 = hash(captured.value(name));
        } else {
            record.state = kind == BackupKind::Legacy ? BackupFileState::NotCaptured : BackupFileState::Missing;
        }
        snapshot.files.append(record);
    }
    QTemporaryDir staging(QDir(root).filePath(".staging-XXXXXX"));
    if (!staging.isValid())
        return fail(error, QString("Cannot stage backup: %1").arg(staging.errorString()));
    const QDir stage(staging.path());
    for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
        if (!writeAtomic(stage.filePath(it.key()), it.value(), error))
            return false;
    }
    if (!writeAtomic(stage.filePath(manifestName), encodeManifest(snapshot), error))
        return false;
    BackupSnapshot verified;
    QMap<QString, QByteArray> staged;
    if (!verifyDirectory(staging.path(), snapshot.id, verified, staged, error))
        return false;
    if (staged != captured)
        return fail(error, "Snapshot verification failed.");
    if (canceled && canceled())
        return fail(error, "Backup canceled.");
    QMap<QString, QByteArray> current;
    if (!readSource(paths, kind, current, error) || current != captured)
        return fail(error, "Source files changed or became unavailable during backup. Close the game and retry.");
    if (!ensureRoot(false, error))
        return false;
    QDir storage(root);
    if (present(storage.filePath(snapshot.id)) ||
        !storage.rename(QFileInfo(staging.path()).fileName(), snapshot.id))
        return fail(error, "Cannot publish completed snapshot.");
    staging.setAutoRemove(false);
    result = verified;
    error.clear();
    return true;
}

bool BackupService::createSnapshot(const QString &label, BackupSnapshot &result, QString &error,
                                    const CancelCheck &canceled) const
{
    return capture(BackupKind::Normal, label, result, error, canceled);
}

bool BackupService::importLegacy(const QString &label, BackupSnapshot &result, QString &error,
                                 const CancelCheck &canceled) const
{
    return capture(BackupKind::Legacy, label, result, error, canceled);
}

bool BackupService::readVerified(const QString &id, BackupSnapshot &result,
                                 QMap<QString, QByteArray> &bytes, QString &error) const
{
    result = {};
    result.id = id;
    bytes.clear();
    QString path;
    if (!snapshotPath(id, path, error)) {
        result.error = error;
        return false;
    }
    return verifyDirectory(path, id, result, bytes, error);
}

bool BackupService::inspect(const QString &id, BackupSnapshot &result, QString &error) const
{
    QMap<QString, QByteArray> discarded;
    return readVerified(id, result, discarded, error);
}

bool BackupService::list(QList<BackupSnapshot> &result, QString &error) const
{
    result.clear();
    if (!ensureRoot(false, error))
        return false;
    if (!QFileInfo::exists(root))
        return true; // No backups yet.
    const QDir dir(root);
    for (const auto &entry : dir.entryInfoList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (!validId(entry.fileName()))
            continue; // No incomplete staging or unrelated files are catalogued.
        BackupSnapshot snapshot;
        QString diagnostic;
        inspect(entry.fileName(), snapshot, diagnostic);
        result.append(snapshot);
    }
    // Timestamp prefix sorts chronologically even for entries with bad manifests.
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.id > b.id; });
    error.clear();
    return true;
}

bool BackupService::renameLabel(const QString &id, const QString &label, QString &error) const
{
    QString path;
    if (!snapshotPath(id, path, error))
        return false;
    QLockFile lock(QDir(root).filePath(".catalog.lock"));
    if (!lock.tryLock(0))
        return fail(error, "Cannot lock backup storage.");
    BackupSnapshot snapshot;
    if (!inspect(id, snapshot, error))
        return false;
    if (snapshot.kind == BackupKind::Safety)
        return fail(error, "Automatic safety snapshots are immutable.");
    snapshot.label = label;
    if (!writeAtomic(QDir(path).filePath(manifestName), encodeManifest(snapshot), error))
        return false;
    error.clear();
    return true;
}

bool BackupService::deleteSnapshot(const QString &id, bool confirmed, QString &error) const
{
    if (!confirmed)
        return fail(error, "Snapshot deletion not confirmed.");
    QString path;
    if (!snapshotPath(id, path, error))
        return false;
    QLockFile lock(QDir(root).filePath(".catalog.lock"));
    if (!lock.tryLock(0))
        return fail(error, "Cannot lock backup storage.");
    const QStringList allowed{manifestName, primaryName, gameBackupName, legacyName};
    if (!snapshotPath(id, path, error) || !safeContents(path, allowed, error))
        return false;
    // Protect safety snapshots even if a captured file is damaged.
    QByteArray manifest;
    BackupSnapshot snapshot;
    QString diagnostic;
    if (!readRegular(QDir(path).filePath(manifestName), manifest, diagnostic) ||
        !decodeManifest(manifest, id, snapshot, diagnostic))
        return fail(error, "Cannot identify snapshot kind safely. Preserve the directory for manual inspection.");
    if (snapshot.kind == BackupKind::Safety)
        return fail(error, "Automatic safety snapshots are immutable.");
    // Never recursively delete arbitrary contents. Unpublish the selected
    // directory first so a partial cleanup cannot look like a usable backup.
    const auto tombstone = ".deleted-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir storage(root);
    if (!storage.rename(id, tombstone))
        return fail(error, "Cannot remove selected snapshot from catalog.");
    const QDir deleted(storage.filePath(tombstone));
    for (const auto &name : allowed) {
        const auto file = deleted.filePath(name);
        if (present(file) && (linked(QFileInfo(file)) || !QFile::remove(file)))
            return fail(error, QString("Snapshot unpublished but cleanup failed: %1").arg(deleted.path()));
    }
    if (!storage.rmdir(tombstone))
        return fail(error, QString("Snapshot unpublished but cleanup failed: %1").arg(deleted.path()));
    error.clear();
    return true;
}
