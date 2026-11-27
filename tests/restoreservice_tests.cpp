#include "restoreservice.h"
#include "savestore.h"
#include "jsonparser.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <QProcess>
#include <windows.h>
#endif

namespace {
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray fixture(const QString &name)
{
    return read(QDir(FIXTURE_DIR).filePath(name));
}

bool closed(QString &error) { error.clear(); return true; }

struct Sandbox
{
    QTemporaryDir temp;
    SavePaths paths{QDir(temp.path()).filePath("saves")};
    QString storage = QDir(temp.path()).filePath("backups");
    BackupService backups{paths, storage};
    RestoreService restores{paths, storage, closed};
    bool setup() { return temp.isValid() && QDir().mkpath(paths.directory()); }
    bool pair(const QByteArray &primary, const QByteArray &companion)
    { return put(paths.primary(), primary) && put(paths.gameBackup(), companion); }
    QString member(const BackupSnapshot &snapshot, const QString &name) const
    { return QDir(storage).filePath(snapshot.id + "/" + name); }
};

const QByteArray multi = "{}~SettingsSplit~"
    "{\"name\":\"First;character\",\"unknown\":123};{};{};opaque;"
    "~NoticeMeSenpaiNoticeMe~"
    "{\"name\":\"Second\",\"memory\":42};{};{};extra;";
}

class RestoreServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void fixtureEditRestoreQualification()
    {
        Sandbox s;
        QVERIFY(s.setup());
        const auto primary = fixture("SaveData.txt");
        const auto companion = fixture("SaveData1.txt");
        QVERIFY(!primary.isEmpty() && !companion.isEmpty());
        QVERIFY(s.pair(primary, companion));
        const auto hash = [](const QByteArray &bytes) {
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        };
        const auto record = [&](const QString &stage) {
            qInfo().noquote() << "QUALIFICATION" << stage
                << "primary=" + QString::fromLatin1(hash(read(s.paths.primary())))
                << "companion=" + QString::fromLatin1(hash(read(s.paths.gameBackup())));
        };
        record("before");
        QString error;
        BackupSnapshot baseline, edited, safety, undo;
        QVERIFY2(s.backups.createSnapshot("Qualification baseline", baseline, error), qPrintable(error));
        QCOMPARE(baseline.files[0].sha256, hash(primary));
        QCOMPARE(baseline.files[1].sha256, hash(companion));
        QCOMPARE(read(s.member(baseline, "SaveData.txt")), primary);
        QCOMPARE(read(s.member(baseline, "SaveData.txt.bak")), companion);
        record("after capture");

        SaveStore store(s.paths);
        QVERIFY2(store.load(error), qPrintable(error));
        const auto original = store.document();
        QCOMPARE(original.characters().size(), 2);
        const int memory = original.characters()[1].character["memory"].toInt();
        QVERIFY2(store.mutateCharacter(1, [memory](SaveCharacter &slot) {
            slot.character["memory"] = memory == 12345 ? 12346 : 12345;
        }, error), qPrintable(error));
        const auto changed = read(s.paths.primary());
        QVERIFY(changed != primary);
        QCOMPARE(read(s.paths.gameBackup()), companion);
        auto expected = original;
        expected.editCharacter(1).character["memory"] = memory == 12345 ? 12346 : 12345;
        QCOMPARE(changed, expected.serialize());
        QCOMPARE(store.document().characters()[0].character, original.characters()[0].character);
        QCOMPARE(store.document().characters()[0].info, original.characters()[0].info);
        QCOMPARE(store.document().characters()[0].quests, original.characters()[0].quests);
        QCOMPARE(store.document().characters()[1].info, original.characters()[1].info);
        QCOMPARE(store.document().characters()[1].quests, original.characters()[1].quests);
        QCOMPARE(store.document().settings(), original.settings());
        record("after edit");
        QVERIFY2(s.backups.createSnapshot("Qualification edited", edited, error), qPrintable(error));
        QCOMPARE(edited.files[0].sha256, hash(changed));

        RestorePreview preview;
        QVERIFY2(s.restores.preview(baseline.id, preview, error), qPrintable(error));
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), primary);
        QCOMPARE(read(s.paths.gameBackup()), companion);
        QCOMPARE(safety.files[0].sha256, hash(changed));
        QCOMPARE(safety.files[1].sha256, hash(companion));
        QCOMPARE(read(s.member(safety, "SaveData.txt")), changed);
        QCOMPARE(read(s.member(safety, "SaveData.txt.bak")), companion);
        record("after restore");
        qInfo().noquote() << "QUALIFICATION safety snapshot"
            << "primary=" + QString::fromLatin1(safety.files[0].sha256)
            << "companion=" + QString::fromLatin1(safety.files[1].sha256);
        // The pre-restore in-memory editor is stale and must not write.
        QVERIFY(!store.canEdit());
        QVERIFY(!store.mutateCharacter(1, [](SaveCharacter &slot) {
            slot.character["memory"] = 1;
        }, error));
        QCOMPARE(read(s.paths.primary()), primary);
        QCOMPARE(read(s.paths.gameBackup()), companion);
        QVERIFY2(store.load(error), qPrintable(error));
        QCOMPARE(store.document().serialize(), primary);
        QCOMPARE(store.document().characters()[1].character["memory"].toInt(), memory);

        QVERIFY2(s.restores.preview(safety.id, preview, error), qPrintable(error));
        QVERIFY2(s.restores.restore(preview, undo, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), changed);
        QCOMPARE(read(s.paths.gameBackup()), companion);
        record("after undo");
        QVERIFY2(s.restores.preview(baseline.id, preview, error), qPrintable(error));
        QVERIFY2(s.restores.restore(preview, undo, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), primary);
        QCOMPARE(read(s.paths.gameBackup()), companion);
        QCOMPARE(read(s.member(baseline, "SaveData.txt")), primary);
        QCOMPARE(read(s.member(edited, "SaveData.txt")), changed);
        QVERIFY(!RestoreService::hasPending(s.paths));
        record("final");
    }

    void fullRestoreAndSafety()
    {
        Sandbox s;
        QVERIFY(s.setup());
        const auto a = fixture("SaveData.txt");
        const auto b = fixture("SaveData1.txt");
        QVERIFY(!a.isEmpty() && !b.isEmpty() && a != b);
        QVERIFY(s.pair(a, b));
        QString error;
        BackupSnapshot snapshotA, snapshotB, safety, safety2;
        QVERIFY2(s.backups.createSnapshot("A", snapshotA, error), qPrintable(error));
        QVERIFY(s.pair(b, a));
        QVERIFY(s.backups.createSnapshot("B", snapshotB, error));
        QVERIFY(put(s.paths.legacyEditorBackup(), "unrelated legacy"));
        QVERIFY(put(QDir(s.paths.directory()).filePath("steam_autocloud.vdf"), "unrelated cloud"));
        RestorePreview preview;
        QVERIFY2(s.restores.preview(snapshotA.id, preview, error), qPrintable(error));
        QVERIFY(!preview.primaryNeedsRecovery);
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), a);
        QCOMPARE(read(s.paths.gameBackup()), b);
        QVERIFY(!RestoreService::hasPending(s.paths));
        QCOMPARE(safety.kind, BackupKind::Safety);
        QCOMPARE(safety.files[0].sha256, snapshotB.files[0].sha256);
        QCOMPARE(safety.files[1].sha256, snapshotB.files[1].sha256);
        QCOMPARE(read(s.member(safety, "SaveData.txt")), b);
        QCOMPARE(read(s.member(safety, "SaveData.txt.bak")), a);
        QVERIFY(!s.backups.renameLabel(safety.id, "changed", error));
        QVERIFY(!s.backups.deleteSnapshot(safety.id, true, error));
        QVERIFY2(s.restores.preview(safety.id, preview, error), qPrintable(error));
        QVERIFY2(s.restores.restore(preview, safety2, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), b);
        QCOMPARE(read(s.paths.gameBackup()), a);
        QCOMPARE(read(s.member(snapshotA, "SaveData.txt")), a);
        QCOMPARE(read(s.member(safety, "SaveData.txt")), b);
        QCOMPARE(read(s.paths.legacyEditorBackup()), QByteArray("unrelated legacy"));
        QCOMPARE(read(QDir(s.paths.directory()).filePath("steam_autocloud.vdf")), QByteArray("unrelated cloud"));
    }

    void optionalAndLegacy()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(put(s.paths.primary(), multi));
        BackupSnapshot absent, safety, legacy;
        QVERIFY2(s.backups.createSnapshot({}, absent, error), qPrintable(error));
        QVERIFY(s.pair(fixture("SaveData1.txt"), "existing companion"));
        RestorePreview preview;
        QVERIFY(s.restores.preview(absent.id, preview, error));
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), multi);
        QVERIFY(!QFileInfo::exists(s.paths.gameBackup()));
        SaveStore reloaded(s.paths);
        QVERIFY2(reloaded.load(error), qPrintable(error));
        QCOMPARE(reloaded.document().characters().size(), 2);
        QVERIFY(put(s.paths.gameBackup(), {}));
        QVERIFY(put(s.paths.legacyEditorBackup(), fixture("SaveData1.txt")));
        QVERIFY(s.backups.importLegacy({}, legacy, error));
        QVERIFY(s.restores.preview(legacy.id, preview, error));
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), fixture("SaveData1.txt"));
        QVERIFY(QFileInfo::exists(s.paths.gameBackup()));
        QVERIFY(read(s.paths.gameBackup()).isEmpty());
        QCOMPARE(read(s.paths.legacyEditorBackup()), fixture("SaveData1.txt"));
    }

    void recoverMissingAndCorruptPrimary()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(s.pair(multi, "selected companion"));
        BackupSnapshot snapshot, safety;
        QVERIFY(s.backups.createSnapshot({}, snapshot, error));
        QVERIFY(QFile::remove(s.paths.primary()));
        QVERIFY(put(s.paths.gameBackup(), "old companion"));
        RestorePreview preview;
        QVERIFY(s.restores.preview(snapshot.id, preview, error));
        QVERIFY(preview.primaryNeedsRecovery);
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(safety.files[0].state, BackupFileState::Missing);
        QCOMPARE(read(s.member(safety, "SaveData.txt.bak")), QByteArray("old companion"));
        QVERIFY(!s.restores.preview(safety.id, preview, error));
        QCOMPARE(read(s.paths.primary()), multi);
        const QByteArray corrupt("broken\0bytes", 12);
        QVERIFY(put(s.paths.primary(), corrupt));
        QVERIFY(QFile::remove(s.paths.gameBackup()));
        QVERIFY(s.restores.preview(snapshot.id, preview, error));
        QVERIFY(preview.primaryNeedsRecovery);
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(read(s.member(safety, "SaveData.txt")), corrupt);
        QCOMPARE(safety.files[1].state, BackupFileState::Missing);
        QVERIFY(!s.restores.preview(safety.id, preview, error));
        QCOMPARE(read(s.paths.primary()), multi);
        // Even a missing save directory can be explicitly recovered.
        QVERIFY(QFile::remove(s.paths.primary()));
        QVERIFY(QFile::remove(s.paths.gameBackup()));
        QVERIFY(QDir().rmdir(s.paths.directory()));
        QVERIFY(s.restores.preview(snapshot.id, preview, error));
        QVERIFY2(s.restores.restore(preview, safety, error), qPrintable(error));
        QCOMPARE(safety.files[0].state, BackupFileState::Missing);
        QCOMPARE(safety.files[1].state, BackupFileState::Missing);
    }

    void invalidSnapshotAndChangedPreview()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(s.pair(multi, "old"));
        BackupSnapshot good, bad, safety;
        QVERIFY(s.backups.createSnapshot({}, good, error));
        QVERIFY(put(s.paths.primary(), "malformed"));
        QVERIFY(s.backups.createSnapshot({}, bad, error));
        RestorePreview preview;
        QVERIFY(!s.restores.preview(bad.id, preview, error));
        QVERIFY(s.restores.preview(good.id, preview, error));
        QVERIFY(put(s.paths.gameBackup(), "externally changed"));
        QVERIFY(!s.restores.restore(preview, safety, error));
        QVERIFY(error.contains("changed"));
        QVERIFY(!safety.valid);
        QCOMPARE(read(s.paths.primary()), QByteArray("malformed"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("externally changed"));
        QVERIFY(!RestoreService::hasPending(s.paths));
        QVERIFY(s.restores.preview(good.id, preview, error));
        QVERIFY(put(s.member(good, "SaveData.txt"), "tamper"));
        QVERIFY(!s.restores.restore(preview, safety, error));
        QVERIFY(error.contains("checksum"));
        QVERIFY(!safety.valid);
        QVERIFY(!RestoreService::hasPending(s.paths));
    }

    void gameAndWriteDenied()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(s.pair(multi, "A"));
        BackupSnapshot chosen, safety;
        QVERIFY(s.backups.createSnapshot({}, chosen, error));
        QVERIFY(s.pair(fixture("SaveData.txt"), "B"));
        RestorePreview preview;
        QVERIFY(s.restores.preview(chosen.id, preview, error));
        RestoreService running(s.paths, s.storage, [](QString &error) {
            error = "game running";
            return false;
        });
        QVERIFY(!running.restore(preview, safety, error));
        QCOMPARE(error, QString("game running"));
        QVERIFY(!safety.valid);

        QLockFile other(s.paths.primary() + ".editor.lock");
        QVERIFY(other.tryLock(0));
        QVERIFY(!s.restores.restore(preview, safety, error));
        QVERIFY(!safety.valid);
        other.unlock();

        // Unavailable safety storage: after preview, make catalog lock a directory.
        const auto catalogLock = QDir(s.storage).filePath(".catalog.lock");
        QVERIFY(QDir().mkdir(catalogLock));
        QVERIFY(!s.restores.restore(preview, safety, error));
        QVERIFY(!safety.valid);
        QVERIFY(QDir().rmdir(catalogLock));
        QVERIFY(!RestoreService::hasPending(s.paths));
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("B"));

#ifdef Q_OS_WIN
        // Real denied companion replacement: another process could hold this
        // handle. Read access is allowed, replacement/delete is not.
        const auto native = QDir::toNativeSeparators(s.paths.gameBackup());
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const bool restored = s.restores.restore(preview, safety, error);
        CloseHandle(handle);
        QVERIFY(!restored);
        QVERIFY(!RestoreService::hasPending(s.paths));
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("B"));
#endif
    }

    void partialReplaceAndCrash()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(s.pair(multi, "A"));
        BackupSnapshot chosen, safety;
        QVERIFY(s.backups.createSnapshot({}, chosen, error));
        // Old primary absent, companion present. Failure after creating primary
        // must remove it again, and preserve the companion.
        QVERIFY(QFile::remove(s.paths.primary()));
        QVERIFY(put(s.paths.gameBackup(), "B"));
        RestorePreview preview;
        QVERIFY(s.restores.preview(chosen.id, preview, error));
        bool observedPartial = false;
        RestoreService failSecond(s.paths, s.storage, closed, [&](int completed, QString &diagnostic) {
            if (completed == 1) {
                observedPartial = read(s.paths.primary()) == multi;
                diagnostic = "injected second replacement failure";
                return false;
            }
            return true;
        });
        QVERIFY(!failSecond.restore(preview, safety, error));
        QVERIFY(observedPartial);
        QVERIFY(error.contains("Original files restored"));
        QVERIFY(!QFileInfo::exists(s.paths.primary()));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("B"));
        QVERIFY(!RestoreService::hasPending(s.paths));

        // Interrupt after the primary commit, before companion creation. The
        // journal has already been published. No catch/rollback inside restore.
        QVERIFY(put(s.paths.primary(), fixture("SaveData.txt")));
        QVERIFY(QFile::remove(s.paths.gameBackup()));
        SaveStore stale(s.paths);
        QVERIFY(stale.load(error));
        QVERIFY(s.restores.preview(chosen.id, preview, error));
        RestoreService crash(s.paths, s.storage, closed, [](int completed, QString &) {
            if (completed == 1) throw std::runtime_error("simulated interruption");
            return true;
        });
        bool interrupted = false;
        try { crash.restore(preview, safety, error); }
        catch (const std::runtime_error &) { interrupted = true; }
        QVERIFY(interrupted);
        QVERIFY(RestoreService::hasPending(s.paths));
        QCOMPARE(read(s.paths.primary()), multi);
        QVERIFY(!QFileInfo::exists(s.paths.gameBackup()));
        QVERIFY(!stale.canEdit());
        QVERIFY(!stale.mutateCharacter(0, [](SaveCharacter &slot) { slot.character["memory"] = 0; }, error));
        SaveStore blocked(s.paths);
        QVERIFY(!blocked.load(error));
        BackupSnapshot another;
        QVERIFY(!s.backups.createSnapshot({}, another, error));
        QVERIFY(!RestoreService::recoverPending(s.paths, error, [](QString &e) { e = "game running"; return false; }));
        QVERIFY(RestoreService::hasPending(s.paths));
#ifdef Q_OS_WIN
        // The real application startup facade performs recovery before loading.
        JsonParser::configure(s.paths);
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QVERIFY(JsonParser::canEdit());
#else
        QVERIFY2(RestoreService::recoverPending(s.paths, error, closed), qPrintable(error));
#endif
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QVERIFY(!QFileInfo::exists(s.paths.gameBackup()));
        QVERIFY(!RestoreService::hasPending(s.paths));
        QVERIFY(s.backups.inspect(safety.id, another, error));

        // Interrupt after BOTH commits, but before final verification and
        // journal cleanup. Rollback must also remove the new companion.
        QVERIFY(s.restores.preview(chosen.id, preview, error));
        RestoreService crashAfterBoth(s.paths, s.storage, closed, [](int completed, QString &) {
            if (completed == 2) throw std::runtime_error("interrupted after both commits");
            return true;
        });
        interrupted = false;
        try { crashAfterBoth.restore(preview, safety, error); }
        catch (const std::runtime_error &) { interrupted = true; }
        QVERIFY(interrupted);
        QVERIFY(RestoreService::hasPending(s.paths));
        QCOMPARE(read(s.paths.primary()), multi);
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("A"));
        QVERIFY2(RestoreService::recoverPending(s.paths, error, closed), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QVERIFY(!QFileInfo::exists(s.paths.gameBackup()));
        QVERIFY(!RestoreService::hasPending(s.paths));
    }

    void externalChangeAndDamagedJournal()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        QVERIFY(s.pair(multi, "A"));
        BackupSnapshot chosen, safety;
        QVERIFY(s.backups.createSnapshot({}, chosen, error));
        QVERIFY(s.pair(fixture("SaveData.txt"), "B"));
        RestorePreview preview;
        QVERIFY(s.restores.preview(chosen.id, preview, error));
        RestoreService changing(s.paths, s.storage, closed, [&](int completed, QString &) {
            return completed != 1 || put(s.paths.gameBackup(), "external");
        });
        QVERIFY(!changing.restore(preview, safety, error));
        QVERIFY(error.contains("external"));
        QVERIFY(RestoreService::hasPending(s.paths));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("external"));
        QCOMPARE(read(s.paths.primary()), multi);
        QVERIFY(!RestoreService::recoverPending(s.paths, error, closed));
        QVERIFY(RestoreService::hasPending(s.paths));
        // Explicitly restore known state to permit retry; no unknown bytes overwritten.
        QVERIFY(put(s.paths.gameBackup(), "B"));
        QVERIFY2(RestoreService::recoverPending(s.paths, error, closed), qPrintable(error));
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("B"));

        QVERIFY(put(RestoreService::journalPath(s.paths), "bad journal"));
        QVERIFY(!RestoreService::recoverPending(s.paths, error, closed));
        QVERIFY(RestoreService::hasPending(s.paths));
        JsonParser::configure(s.paths);
        QVERIFY(!JsonParser::init(error));
        QVERIFY(!JsonParser::canEdit());
        QVERIFY(JsonParser::characters().isEmpty());
        QCOMPARE(read(s.paths.primary()), fixture("SaveData.txt"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("B"));
    }

#ifdef Q_OS_WIN
    void actualProcessGuard()
    {
        QString error;
        // A disposable test-only executable has the installed game's basename.
        // The real game is never launched or terminated by these tests.
        QVERIFY2(RestoreService::gameClosed(error), qPrintable(error));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const SavePaths paths(dir.path());
        const QByteArray original = R"({}~SettingsSplit~{"name":"Guard","memory":10};{};{};opaque;)";
        QVERIFY(put(paths.primary(), original));
        SaveStore store(paths);
        QVERIFY2(store.load(error), qPrintable(error));
        QProcess child;
        child.start(QDir(QCoreApplication::applicationDirPath()).filePath("TLHON.exe"), QStringList{});
        QVERIFY(child.waitForStarted());
        const bool blocked = !RestoreService::gameClosed(error);
        const bool stillRunning = child.state() == QProcess::Running;
        const bool canEdit = store.canEdit();
        QString editError;
        const bool mutated = store.mutateCharacter(0, [](SaveCharacter &slot) {
            slot.character["memory"] = 999;
        }, editError);
        child.kill();
        QVERIFY(child.waitForFinished());
        QVERIFY(blocked);
        QVERIFY(stillRunning);
        QVERIFY(!canEdit && !mutated);
        QCOMPARE(read(paths.primary()), original);
        QVERIFY(editError.contains("TLHON.exe"));
        QVERIFY(error.contains("TLHON.exe"));
        QVERIFY2(RestoreService::gameClosed(error), qPrintable(error));
    }
#endif
};

QTEST_GUILESS_MAIN(RestoreServiceTests)
#include "restoreservice_tests.moc"
