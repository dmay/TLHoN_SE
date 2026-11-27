#include "restoreservice.h"
#include "savedocument.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <array>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
using Pair = QMap<QString, QByteArray>;
const QString primary = "SaveData.txt";
const QString companion = "SaveData.txt.bak";
const std::array<QString, 2> names{primary, companion};

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

bool present(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || linked(info);
}

bool readRegular(const QString &path, QByteArray &bytes, QString &error)
{
    const QFileInfo before(path);
    if (linked(before) || !before.isFile())
        return fail(error, QString("Not a regular file: %1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QString("Cannot read %1: %2").arg(path, file.errorString()));
    bytes = file.readAll();
    QFileInfo after(path);
    if (file.error() != QFileDevice::NoError || !after.isFile() || linked(after) ||
        bytes.size() != before.size() || after.size() != before.size() ||
        after.lastModified() != before.lastModified())
        return fail(error, QString("File changed or could not be read: %1").arg(path));
    return true;
}

bool readPair(const SavePaths &paths, Pair &pair, QString &error)
{
    pair.clear();
    for (const auto &name : names) {
        const auto path = QDir(paths.directory()).filePath(name);
        if (present(path)) {
            QByteArray bytes;
            if (!readRegular(path, bytes, error)) return false;
            pair.insert(name, bytes);
        }
    }
    return true;
}

bool sameFile(const Pair &a, const Pair &b, const QString &name)
{
    return a.contains(name) == b.contains(name) && a.value(name) == b.value(name);
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject encodePair(const Pair &pair)
{
    QJsonObject result;
    for (const auto &name : names) {
        if (pair.contains(name)) {
            const auto bytes = pair.value(name);
            result[name] = QJsonObject{{"bytes", QString::fromLatin1(bytes.toBase64())},
                                       {"sha256", digest(bytes)}};
        } else result[name] = QJsonValue(QJsonValue::Null);
    }
    return result;
}

bool decodePair(const QJsonValue &value, Pair &pair)
{
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    if (object.size() != int(names.size())) return false;
    pair.clear();
    for (const auto &name : names) {
        const auto record = object.value(name);
        if (record.isNull()) continue;
        if (!record.isObject()) return false;
        const auto data = record.toObject();
        if (!data.value("bytes").isString() || !data.value("sha256").isString()) return false;
        const auto encoded = data.value("bytes").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64(encoded);
        if (decoded.toBase64() != encoded || digest(decoded) != data.value("sha256").toString()) return false;
        pair.insert(name, decoded);
    }
    return true;
}

bool loadJournal(const SavePaths &paths, Pair &before, Pair &after, QString &error)
{
    QByteArray bytes;
    if (!readRegular(RestoreService::journalPath(paths), bytes, error)) return false;
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (!document.isObject() || object.value("version") != QJsonValue(1) ||
        object.value("target").toString() != paths.directory() ||
        !decodePair(object.value("before"), before) || !decodePair(object.value("after"), after) ||
        !after.contains(primary))
        return fail(error, "Invalid restore journal; automatic recovery is unsafe.");
    SaveDocument parsed;
    if (!SaveDocument::parse(after.value(primary), parsed, error)) return false;
    return true;
}

bool writeJournal(const SavePaths &paths, const Pair &before, const Pair &after,
                  const BackupSnapshot &safety, const QString &storage, QString &error)
{
    const auto path = RestoreService::journalPath(paths);
    if (present(path)) return fail(error, "A restore journal already exists.");
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"target", paths.directory()},
        {"before", encodePair(before)}, {"after", encodePair(after)},
        {"safetyId", safety.id}, {"safetyStorage", storage}}).toJson(QJsonDocument::Compact);
    QSaveFile journal(path);
    journal.setDirectWriteFallback(false);
    if (!journal.open(QIODevice::WriteOnly) || journal.write(bytes) != bytes.size() || !journal.commit())
        return fail(error, QString("Cannot publish restore journal: %1").arg(journal.errorString()));
    Pair verifiedBefore, verifiedAfter;
    if (!loadJournal(paths, verifiedBefore, verifiedAfter, error) ||
        verifiedBefore != before || verifiedAfter != after)
        return fail(error, "Restore journal failed verification.");
    return true;
}

// Stage every replacement before modifying any active file. Recheck the whole
// pair before each operation and after the final one, including missing states.
// The game/Steam do not participate in the editor lock; no two-file filesystem
// transaction is available, so the durable journal is our rollback record.
bool applyPair(const SavePaths &paths, const Pair &start, const Pair &desired,
               const RestoreService::GameCheck &gameCheck,
               const RestoreService::StepCheck &stepCheck, QString &error)
{
    std::array<std::unique_ptr<QSaveFile>, 2> staged;
    for (size_t i = 0; i < names.size(); ++i) {
        const auto &name = names[i];
        if (sameFile(start, desired, name)) continue;
        const auto path = QDir(paths.directory()).filePath(name);
        const QFileInfo info(path);
        if (present(path) && (linked(info) || !info.isFile() || !info.isWritable()))
            return fail(error, QString("Target is not a regular writable file: %1").arg(path));
        if (!desired.contains(name)) continue;
        auto file = std::make_unique<QSaveFile>(path);
        file->setDirectWriteFallback(false);
        const auto bytes = desired.value(name);
        if (!file->open(QIODevice::WriteOnly) || file->write(bytes) != bytes.size())
            return fail(error, QString("Cannot stage %1: %2").arg(path, file->errorString()));
        staged[i] = std::move(file);
    }

    Pair expected = start;
    int completed = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const auto &name = names[i];
        if (sameFile(start, desired, name)) continue;
        if (stepCheck && !stepCheck(completed, error)) return false;
        if (!gameCheck(error)) return false;
        Pair actual;
        if (!readPair(paths, actual, error)) return false;
        if (actual != expected) return fail(error, "Active save files changed externally during restore.");
        const auto path = QDir(paths.directory()).filePath(name);
        if (staged[i]) {
            if (!staged[i]->commit())
                return fail(error, QString("Cannot replace %1: %2").arg(path, staged[i]->errorString()));
            expected.insert(name, desired.value(name));
        } else {
            // Only remove an explicitly absent file after journal and safety
            // snapshot validation; never remove the primary to replace it.
            if (!QFile::remove(path)) return fail(error, QString("Cannot remove %1").arg(path));
            expected.remove(name);
        }
        ++completed;
    }
    Pair actual;
    if (!readPair(paths, actual, error)) return false;
    if (actual != desired) return fail(error, "Final restored files do not match the expected bytes.");
    return true;
}

bool removeJournal(const SavePaths &paths, QString &error)
{
    const auto path = RestoreService::journalPath(paths);
    if (linked(QFileInfo(path)) || !QFile::remove(path))
        return fail(error, QString("Could not clear restore journal: %1").arg(path));
    error.clear();
    return true;
}

bool recoverLocked(const SavePaths &paths, const RestoreService::GameCheck &check, QString &error)
{
    if (!check(error)) return false;
    Pair before, after, actual;
    if (!loadJournal(paths, before, after, error) || !readPair(paths, actual, error)) return false;
    for (const auto &name : names) {
        if (!sameFile(actual, before, name) && !sameFile(actual, after, name))
            return fail(error, "Active files contain an external change; refusing to overwrite it automatically.");
    }
    if (!applyPair(paths, actual, before, check, {}, error)) return false;
    return removeJournal(paths, error);
}

QString recoveryMessage(const SavePaths &paths, const QString &cause)
{
    return cause + "\nRestore recovery is pending. Close the game and finish Steam Cloud sync, "
        "then use Reload save or restart the editor to retry recovery. If it still fails, preserve "
        "the active files, journal and safety snapshot; consult docs/restore-recovery.md. Journal: "
        + RestoreService::journalPath(paths);
}
}

RestoreService::RestoreService(SavePaths paths, const QString &storageDirectory,
                               GameCheck gameCheck, StepCheck stepCheck)
    : paths(std::move(paths)), backups(this->paths, storageDirectory),
      checkGame(gameCheck ? std::move(gameCheck) : GameCheck(gameClosed)),
      checkStep(std::move(stepCheck))
{}

QString RestoreService::journalPath(const SavePaths &paths)
{
    return paths.primary() + ".editor-restore.json";
}

bool RestoreService::hasPending(const SavePaths &paths)
{
    return present(journalPath(paths));
}

bool RestoreService::gameClosed(QString &error)
{
#ifdef Q_OS_WIN
    // Steam app 1766100, Windows depot 1766101: TLHON.exe.
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE)
        return fail(error, "Could not check running processes. Restore blocked.");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(processes, &entry)) {
        CloseHandle(processes);
        return fail(error, "Could not enumerate running processes. Restore blocked.");
    }
    do {
        if (QString::fromWCharArray(entry.szExeFile).compare("TLHON.exe", Qt::CaseInsensitive) == 0) {
            CloseHandle(processes);
            return fail(error, "The game (TLHON.exe) is running. Close it before restoring.");
        }
    } while (Process32NextW(processes, &entry));
    const auto enumerationError = GetLastError();
    CloseHandle(processes);
    if (enumerationError != ERROR_NO_MORE_FILES)
        return fail(error, "Could not finish checking running processes. Restore blocked.");
    error.clear();
    return true;
#else
    return fail(error, "Automatic game process detection is supported only on Windows. Restore blocked.");
#endif
}

bool RestoreService::preview(const QString &id, RestorePreview &result, QString &error) const
{
    result = {};
    if (hasPending(paths)) return fail(error, recoveryMessage(paths, "An interrupted restore must be recovered first."));
    Pair captured;
    if (!backups.readVerified(id, result.snapshot, captured, error)) return false;
    const bool legacy = result.snapshot.kind == BackupKind::Legacy;
    const auto name = legacy ? QString("SaveData.txt_bckp") : primary;
    SaveDocument parsed;
    if (!captured.contains(name) || !SaveDocument::parse(captured.value(name), parsed, error))
        return fail(error, "Selected snapshot has no parseable primary save. " + error);
    if (!readPair(paths, result.before, error)) return false;
    result.after = legacy ? result.before : captured;
    result.after.insert(primary, captured.value(name));
    result.targetDirectory = paths.directory();
    result.primaryNeedsRecovery = !result.before.contains(primary) ||
        !SaveDocument::parse(result.before.value(primary), parsed, error);
    error.clear();
    return true;
}

bool RestoreService::restore(const RestorePreview &confirmed, BackupSnapshot &safety, QString &error) const
{
    safety = {};
    if (!checkGame(error)) return false;
    if (confirmed.targetDirectory != paths.directory())
        return fail(error, "Restore target changed; confirm again.");
    if (!QDir().mkpath(paths.directory())) return fail(error, "Cannot create save directory.");
    QLockFile lock(paths.primary() + ".editor.lock");
    if (!lock.tryLock(0)) return fail(error, "Cannot lock save directory. Close other editor instances.");

    RestorePreview current;
    if (!preview(confirmed.snapshot.id, current, error)) return false;
    if (current.before != confirmed.before || current.after != confirmed.after ||
        current.snapshot.kind != confirmed.snapshot.kind)
        return fail(error, "Active files or selected snapshot changed since confirmation. Preview again.");
    if (!backups.capture(BackupKind::Safety, "Automatic pre-restore safety snapshot", safety, error, {}, true))
        return false;
    Pair safetyBytes;
    BackupSnapshot verified;
    if (!backups.readVerified(safety.id, verified, safetyBytes, error) || safetyBytes != current.before)
        return fail(error, "Safety snapshot does not match the confirmed current bytes. Restore aborted.");
    Pair actual;
    if (!checkGame(error) || !readPair(paths, actual, error)) return false;
    if (actual != current.before) return fail(error, "Active files changed after the safety capture. Restore aborted.");

    if (!writeJournal(paths, current.before, current.after, safety, backups.storageDirectory(), error)) {
        if (hasPending(paths)) error = recoveryMessage(paths, error);
        return false;
    }
    if (applyPair(paths, current.before, current.after, checkGame, checkStep, error) &&
        removeJournal(paths, error))
        return true;

    const QString failure = error;
    QString rollbackError;
    if (recoverLocked(paths, checkGame, rollbackError))
        return fail(error, failure + "\nOriginal files restored. Safety snapshot kept: " + safety.id);
    return fail(error, recoveryMessage(paths, failure + "\nRollback could not finish: " + rollbackError +
                                     "\nSafety snapshot: " + backups.storageDirectory() + "/" + safety.id));
}

bool RestoreService::recoverPending(const SavePaths &paths, QString &error, GameCheck gameCheck)
{
    error.clear();
    if (!hasPending(paths)) return true;
    QLockFile lock(paths.primary() + ".editor.lock");
    if (!lock.tryLock(0)) return fail(error, recoveryMessage(paths, "Cannot lock save directory for recovery."));
    if (!hasPending(paths)) return true;
    const GameCheck check = gameCheck ? std::move(gameCheck) : GameCheck(gameClosed);
    if (!recoverLocked(paths, check, error)) {
        error = recoveryMessage(paths, error);
        return false;
    }
    return true;
}
