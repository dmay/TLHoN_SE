#include "savestore.h"
#include "restoreservice.h"

#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <utility>

namespace {
bool readFile(const QString &path, QByteArray &bytes, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QString("Cannot open save '%1': %2").arg(path, file.errorString());
        return false;
    }
    bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        error = QString("Cannot read save '%1': %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool writable(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink() && info.isWritable();
}

bool unchanged(const QString &path, const QByteArray &expected, QString &error)
{
    QByteArray actual;
    if (!readFile(path, actual, error))
        return false;
    if (actual != expected) {
        error = QString("Save '%1' changed externally. Close the game and reload the save before editing.").arg(path);
        return false;
    }
    if (!writable(path)) {
        error = QString("Save '%1' is read-only or not a regular writable file.").arg(path);
        return false;
    }
    return true;
}
}

SaveStore::SaveStore(SavePaths paths) : savePaths(std::move(paths)) {}

bool SaveStore::canEdit() const
{
    if (!loaded || RestoreService::hasPending(savePaths) || current.characters().isEmpty() || !writable(savePaths.primary()))
        return false;
    QString error;
#ifdef Q_OS_WIN
    if (!RestoreService::gameClosed(error)) return false;
#endif
    QByteArray actual;
    return readFile(savePaths.primary(), actual, error) && actual == loadedBytes;
}

bool SaveStore::load(QString &error)
{
    // Retain the last good document on failure, but do not allow edits to it.
    loaded = false;
    if (RestoreService::hasPending(savePaths)) {
        error = "Interrupted restore must be recovered before loading or editing.";
        return false;
    }
    QByteArray bytes;
    SaveDocument candidate;
    if (!readFile(savePaths.primary(), bytes, error))
        return false;
    if (!SaveDocument::parse(bytes, candidate, error)) {
        error = QString("Invalid save '%1': %2").arg(savePaths.primary(), error);
        return false;
    }
    current = std::move(candidate);
    loadedBytes = std::move(bytes);
    loaded = true;
    if (!writable(savePaths.primary())) {
        error = QString("Save '%1' is read-only. Editing is disabled.").arg(savePaths.primary());
    } else {
        error.clear();
    }
    return true;
}

bool SaveStore::mutateCharacter(int index, const std::function<void(SaveCharacter &)> &mutation, QString &error)
{
    return mutateCharacterChecked(index, [&](SaveCharacter &slot, QString &) {
        mutation(slot);
        return true;
    }, error);
}

bool SaveStore::mutateCharacterChecked(int index, const CheckedMutation &mutation, QString &error)
{
    const QString path = savePaths.primary();
    if (!loaded || !current.hasCharacter(index)) {
        error = "No valid selected character. Reload or recover the save first.";
        return false;
    }
#ifdef Q_OS_WIN
    if (!RestoreService::gameClosed(error)) return false;
#endif
    // Serialize concurrent writes by cooperating editor instances. The game
    // does not participate in this lock: always close it before editing.
    QLockFile lock(path + ".editor.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = QString("Cannot lock '%1' for editing. Check directory permissions and other editor instances.").arg(path);
        return false;
    }
    if (RestoreService::hasPending(savePaths)) {
        loaded = false;
        error = "Interrupted restore must be recovered before editing.";
        return false;
    }
    if (!unchanged(path, loadedBytes, error))
        return false;

    SaveDocument candidate = current;
    error.clear();
    if (!mutation(candidate.editCharacter(index), error)) {
        if (error.isEmpty()) error = "Character mutation rejected.";
        return false;
    }
    const QByteArray bytes = candidate.serialize();
    // Check the complete envelope before staging any write.
    SaveDocument committed;
    if (!SaveDocument::parse(bytes, committed, error))
        return false;
    if (bytes == loadedBytes) {
        error.clear();
        return true;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QString("Cannot stage save '%1': %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        error = QString("Cannot write save '%1': %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    // Check once more immediately before the atomic replacement.
    if (!unchanged(path, loadedBytes, error)
#ifdef Q_OS_WIN
        || !RestoreService::gameClosed(error)
#endif
    ) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = QString("Cannot replace save '%1': %2").arg(path, file.errorString());
        return false;
    }
    current = std::move(committed);
    loadedBytes = bytes;
    error.clear();
    return true;
}
