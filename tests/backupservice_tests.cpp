#include "backupservice.h"
#include "savedocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
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

QString member(const BackupService &service, const BackupSnapshot &snapshot, const QString &name)
{
    return QDir(service.storageDirectory()).filePath(snapshot.id + "/" + name);
}

struct Sandbox
{
    QTemporaryDir temp;
    SavePaths paths{QDir(temp.path()).filePath("saves")};
    BackupService backups{paths, QDir(temp.path()).filePath("backups")};
    bool setup() { return temp.isValid() && QDir().mkpath(paths.directory()); }
};
}

class BackupServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void twoSnapshotsAndRestart()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        const auto primary = fixture("SaveData.txt");
        const auto secondary = fixture("SaveData1.txt");
        QVERIFY(!primary.isEmpty() && !secondary.isEmpty());
        QVERIFY(put(s.paths.primary(), primary));
        QVERIFY(put(s.paths.gameBackup(), secondary));
        QVERIFY(put(s.paths.legacyEditorBackup(), "legacy"));
        QVERIFY(put(QDir(s.paths.directory()).filePath("steam_autocloud.vdf"), "other"));
        BackupSnapshot first, second;
        QVERIFY2(s.backups.createSnapshot("first", first, error), qPrintable(error));
        QVERIFY2(s.backups.createSnapshot("second", second, error), qPrintable(error));
        QVERIFY(first.id != second.id);
        QCOMPARE(first.sourceDirectory, s.paths.directory());
        QVERIFY(first.createdAt.isValid());
        QCOMPARE(first.createdAt.timeSpec(), Qt::UTC);
        QVERIFY(first.valid && second.valid);
        for (const auto &snapshot : {first, second}) {
            QCOMPARE(read(member(s.backups, snapshot, "SaveData.txt")), primary);
            QCOMPARE(read(member(s.backups, snapshot, "SaveData.txt.bak")), secondary);
            QVERIFY(!QFileInfo::exists(member(s.backups, snapshot, "SaveData.txt_bckp")));
            QVERIFY(!QFileInfo::exists(member(s.backups, snapshot, "steam_autocloud.vdf")));
        }
        QCOMPARE(read(s.paths.primary()), primary);
        QCOMPARE(read(s.paths.gameBackup()), secondary);
        QCOMPARE(read(s.paths.legacyEditorBackup()), QByteArray("legacy"));

        const BackupService reopened(s.paths, s.backups.storageDirectory());
        QList<BackupSnapshot> list;
        QVERIFY2(reopened.list(list, error), qPrintable(error));
        QCOMPARE(list.size(), 2);
        QVERIFY(list[0].id > list[1].id);
        QMap<QString, QByteArray> bytes;
        BackupSnapshot inspected;
        QVERIFY2(reopened.readVerified(first.id, inspected, bytes, error), qPrintable(error));
        QCOMPARE(bytes.value("SaveData.txt"), primary);
        QCOMPARE(bytes.value("SaveData.txt.bak"), secondary);
        QCOMPARE(inspected.label, QString("first"));
        QCOMPARE(inspected.files[0].size, qint64(primary.size()));
        QCOMPARE(inspected.files[0].sha256.size(), 64);
        QVERIFY2(reopened.renameLabel(first.id, "renamed", error), qPrintable(error));
        QVERIFY(reopened.inspect(first.id, inspected, error));
        QCOMPARE(inspected.label, QString("renamed"));
        QCOMPARE(read(member(s.backups, first, "SaveData.txt")), primary);
        QVERIFY(!reopened.deleteSnapshot(first.id, false, error));
        QVERIFY(reopened.inspect(first.id, inspected, error));
        QVERIFY2(reopened.deleteSnapshot(first.id, true, error), qPrintable(error));
        QVERIFY(!reopened.inspect(first.id, inspected, error));
        QVERIFY(reopened.inspect(second.id, inspected, error));
        QCOMPARE(read(s.paths.primary()), primary);
        QCOMPARE(read(s.paths.gameBackup()), secondary);
    }

    void optionalAndLegacyAndMalformed()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        BackupSnapshot result;
        QVERIFY(!s.backups.createSnapshot({}, result, error));
        // Opaque malformed bytes still deserve a backup. Restore is separately
        // responsible for parsing, and must distinguish integrity from validity.
        const QByteArray malformed("not a save;\0binary", 18);
        QVERIFY(put(s.paths.primary(), malformed));
        QVERIFY2(s.backups.createSnapshot({}, result, error), qPrintable(error));
        QCOMPARE(result.files[1].state, BackupFileState::Missing);
        QVERIFY(!QFileInfo::exists(member(s.backups, result, "SaveData.txt.bak")));
        QCOMPARE(read(member(s.backups, result, "SaveData.txt")), malformed);
        SaveDocument doc;
        QVERIFY(!SaveDocument::parse(malformed, doc, error));

        QVERIFY(put(s.paths.legacyEditorBackup(), fixture("SaveData1.txt")));
        QVERIFY(put(s.paths.gameBackup(), "not from legacy backup"));
        BackupSnapshot legacy;
        QVERIFY2(s.backups.importLegacy("old editor", legacy, error), qPrintable(error));
        QCOMPARE(legacy.kind, BackupKind::Legacy);
        QCOMPARE(legacy.files[0].name, QString("SaveData.txt_bckp"));
        QCOMPARE(legacy.files[1].state, BackupFileState::NotCaptured);
        QVERIFY(!QFileInfo::exists(member(s.backups, legacy, "SaveData.txt.bak")));
        QCOMPARE(read(member(s.backups, legacy, "SaveData.txt_bckp")), fixture("SaveData1.txt"));
        QCOMPARE(read(s.paths.legacyEditorBackup()), fixture("SaveData1.txt"));
        QCOMPARE(read(s.paths.gameBackup()), QByteArray("not from legacy backup"));

        // Empty-but-present companion must not be mistaken for missing.
        QVERIFY(put(s.paths.gameBackup(), {}));
        BackupSnapshot empty;
        QVERIFY2(s.backups.createSnapshot({}, empty, error), qPrintable(error));
        QCOMPARE(empty.files[1].state, BackupFileState::Present);
        QCOMPARE(empty.files[1].size, 0);
        QVERIFY(QFileInfo::exists(member(s.backups, empty, "SaveData.txt.bak")));
    }

    void syntheticMultiCharacter()
    {
        Sandbox s;
        QVERIFY(s.setup());
        const QByteArray slot = "{\"name\":\"A;B\",\"unknown\":123};{};{};opaque;";
        const QByteArray multi = "{}~SettingsSplit~" + slot + "~NoticeMeSenpaiNoticeMe~" + slot;
        SaveDocument doc;
        QString error;
        QVERIFY2(SaveDocument::parse(multi, doc, error), qPrintable(error));
        QCOMPARE(doc.characters().size(), 2);
        QVERIFY(put(s.paths.primary(), multi));
        BackupSnapshot snapshot;
        QVERIFY2(s.backups.createSnapshot({}, snapshot, error), qPrintable(error));
        QCOMPARE(read(member(s.backups, snapshot, "SaveData.txt")), multi);
    }

    void cancelAndConcurrentChange()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QVERIFY(put(s.paths.primary(), "initial"));
        QVERIFY(put(s.paths.gameBackup(), "companion"));
        QString error;
        BackupSnapshot result;
        QList<BackupSnapshot> list;
        QVERIFY(!s.backups.createSnapshot({}, result, error, [] { return true; }));
        QVERIFY(!result.valid);
        int calls = 0;
        // Cancellation occurs after staging, before publication.
        QVERIFY(!s.backups.createSnapshot({}, result, error, [&] { return ++calls == 2; }));
        QVERIFY(error.contains("canceled"));
        QVERIFY(s.backups.list(list, error));
        QVERIFY(list.isEmpty());
        QCOMPARE(QDir(s.backups.storageDirectory()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);

        // Deterministic external changes between capture and final check.
        for (int change = 0; change < 3; ++change) {
            calls = 0;
            QVERIFY(put(s.paths.primary(), "initial"));
            QVERIFY(put(s.paths.gameBackup(), "companion"));
            bool changed = false;
            QVERIFY(!s.backups.createSnapshot({}, result, error, [&] {
                if (++calls == 2) {
                    if (change == 0) changed = put(s.paths.primary(), "external");
                    else if (change == 1) changed = put(s.paths.gameBackup(), "external");
                    else changed = QFile::remove(s.paths.gameBackup());
                }
                return false;
            }));
            QVERIFY(changed);
            QVERIFY(error.contains("changed"));
            QVERIFY(s.backups.list(list, error));
            QVERIFY(list.isEmpty());
        }
        // Also detect an optional companion appearing after capture.
        calls = 0;
        bool added = false;
        QVERIFY(!s.backups.createSnapshot({}, result, error, [&] {
            if (++calls == 2) added = put(s.paths.gameBackup(), "new");
            return false;
        }));
        QVERIFY(added);
        QVERIFY(error.contains("changed"));
        QVERIFY(s.backups.list(list, error));
        QVERIFY(list.isEmpty());

        QLockFile other(s.paths.primary() + ".editor.lock");
        QVERIFY(other.tryLock(0));
        QVERIFY(!s.backups.createSnapshot({}, result, error));
        QVERIFY(error.contains("lock"));
        QVERIFY(s.backups.list(list, error));
        QVERIFY(list.isEmpty());
    }

    void corruptAndIncomplete()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QVERIFY(put(s.paths.primary(), "original"));
        QString error;
        BackupSnapshot good, corrupt, incomplete, malformed, invalid;
        QVERIFY2(s.backups.createSnapshot({}, good, error), qPrintable(error));
        QVERIFY(s.backups.createSnapshot({}, corrupt, error));
        QVERIFY(s.backups.createSnapshot({}, incomplete, error));
        QVERIFY(s.backups.createSnapshot({}, malformed, error));
        QVERIFY(put(member(s.backups, corrupt, "SaveData.txt"), "tampered")); // same length
        QVERIFY(QFile::remove(member(s.backups, incomplete, "SaveData.txt")));
        QVERIFY(put(member(s.backups, malformed, "manifest.json"), "{invalid"));
        QVERIFY(QDir(s.backups.storageDirectory()).mkdir(".staging-abandoned"));
        QList<BackupSnapshot> list;
        QVERIFY(s.backups.list(list, error));
        QCOMPARE(list.size(), 4);
        int valid = 0;
        for (const auto &snapshot : list) {
            if (snapshot.valid) ++valid;
            else QVERIFY(!snapshot.error.isEmpty());
        }
        QCOMPARE(valid, 1);
        QMap<QString, QByteArray> bytes;
        QVERIFY(!s.backups.readVerified(corrupt.id, invalid, bytes, error));
        QVERIFY(bytes.isEmpty());
        QVERIFY(error.contains("checksum"));
        QVERIFY(!s.backups.renameLabel(malformed.id, "not allowed", error));
        QVERIFY(s.backups.inspect(good.id, invalid, error));
        QVERIFY2(s.backups.deleteSnapshot(incomplete.id, true, error), qPrintable(error));
        QVERIFY(s.backups.inspect(good.id, invalid, error));
        QVERIFY(!s.backups.inspect(incomplete.id, invalid, error));
    }

    void invalidPathsAndStoragePreference()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QString error;
        BackupSnapshot result;
        QVERIFY(put(s.paths.primary(), "original"));
        QSettings settings(QDir(s.temp.path()).filePath("app.ini"), QSettings::IniFormat);
        QVERIFY2(BackupService::configureStorageDirectory(settings, s.paths,
            s.backups.storageDirectory(), error), qPrintable(error));
        QSettings reopened(settings.fileName(), QSettings::IniFormat);
        QCOMPARE(BackupService::configuredStorageDirectory(reopened), s.backups.storageDirectory());
        QVERIFY(!BackupService::configureStorageDirectory(settings, s.paths, s.paths.directory(), error));
        QVERIFY(!BackupService::configureStorageDirectory(settings, s.paths,
            QDir(s.paths.directory()).filePath("backups"), error));
        QCOMPARE(BackupService::configuredStorageDirectory(settings), s.backups.storageDirectory());
        QVERIFY(!BackupService::configureStorageDirectory(settings, s.paths, s.temp.path(), error));
        // Unavailable storage / write staging impossible: root is a file.
        const auto unavailable = QDir(s.temp.path()).filePath("not-a-directory");
        QVERIFY(put(unavailable, "unrelated"));
        BackupService blocked(s.paths, unavailable);
        QVERIFY(!blocked.createSnapshot({}, result, error));
        QCOMPARE(read(unavailable), QByteArray("unrelated"));
        QList<BackupSnapshot> resultList;
        QVERIFY(!blocked.list(resultList, error));

        QVERIFY2(s.backups.createSnapshot({}, result, error), qPrintable(error));
        QVERIFY(!s.backups.deleteSnapshot("../saves", true, error));
        QVERIFY(!s.backups.deleteSnapshot(s.paths.directory(), true, error));
        QVERIFY(!s.backups.deleteSnapshot(result.id + "/..", true, error));
        const auto unexpected = member(s.backups, result, "user-file");
        QVERIFY(put(unexpected, "keep this"));
        QVERIFY(!s.backups.deleteSnapshot(result.id, true, error));
        QCOMPARE(read(unexpected), QByteArray("keep this"));
        QCOMPARE(read(s.paths.primary()), QByteArray("original"));
        QVERIFY(QFile::remove(unexpected));
        QVERIFY2(s.backups.deleteSnapshot(result.id, true, error), qPrintable(error));
    }

    void rejectLinkedEntries()
    {
#ifdef Q_OS_WIN
        QSKIP("QFile::link creates Windows shortcuts rather than filesystem symlinks.");
#else
        Sandbox s;
        QVERIFY(s.setup());
        QVERIFY(put(s.paths.primary(), "original"));
        QString error;
        BackupSnapshot snapshot, inspected;
        QVERIFY2(s.backups.createSnapshot({}, snapshot, error), qPrintable(error));
        const auto captured = member(s.backups, snapshot, "SaveData.txt");
        QVERIFY(QFile::remove(captured));
        QVERIFY(QFile::link(s.paths.primary(), captured));
        QVERIFY(!s.backups.inspect(snapshot.id, inspected, error));
        QVERIFY(!s.backups.deleteSnapshot(snapshot.id, true, error));
        QCOMPARE(read(s.paths.primary()), QByteArray("original"));

        const auto alias = QDir(s.temp.path()).filePath("alias");
        QVERIFY(QFile::link(s.paths.directory(), alias));
        BackupService aliased(s.paths, QDir(alias).filePath("backups"));
        QVERIFY(!aliased.createSnapshot({}, inspected, error));
        QVERIFY(!QFileInfo::exists(QDir(s.paths.directory()).filePath("backups")));
#endif
    }

    void malformedManifestPaths()
    {
        Sandbox s;
        QVERIFY(s.setup());
        QVERIFY(put(s.paths.primary(), "original"));
        QString error;
        BackupSnapshot snapshot, inspected;
        QVERIFY2(s.backups.createSnapshot({}, snapshot, error), qPrintable(error));
        const auto manifest = member(s.backups, snapshot, "manifest.json");
        auto object = QJsonDocument::fromJson(read(manifest)).object();
        auto files = object.value("files").toArray();
        auto entry = files[0].toObject();
        entry["name"] = "../../saves/SaveData.txt";
        files[0] = entry;
        object["files"] = files;
        QVERIFY(put(manifest, QJsonDocument(object).toJson()));
        QVERIFY(!s.backups.inspect(snapshot.id, inspected, error));
        QVERIFY(error.contains("filename"));
        QCOMPARE(read(s.paths.primary()), QByteArray("original"));
    }

};

QTEST_GUILESS_MAIN(BackupServiceTests)
#include "backupservice_tests.moc"
