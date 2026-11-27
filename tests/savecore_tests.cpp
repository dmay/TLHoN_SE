#include "savestore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
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
const QByteArray marker = "~NoticeMeSenpaiNoticeMe~";
const QByteArray slot = R"({"name":"hello;~SettingsSplit~;~NoticeMeSenpaiNoticeMe~","unknown":{"x":"\"}\\;"},"memory":1};{"a":1};{"q":2};{"misc":"~NoticeMeSenpaiNoticeMe~;"};opaque data;)";
}

class SaveCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void pathResolution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SavePaths paths(dir.path());
        QCOMPARE(paths.primary(), QDir(dir.path()).filePath("SaveData.txt"));
        QCOMPARE(paths.gameBackup(), paths.primary() + ".bak");
        QCOMPARE(paths.legacyEditorBackup(), paths.primary() + "_bckp");
    }

    void roundtrip_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("count");
        QTest::newRow("single") << QString("SaveData1.txt") << 1;
        QTest::newRow("multi") << QString("SaveData.txt") << 2;
    }
    void roundtrip()
    {
        QFETCH(QString, file);
        QFETCH(int, count);
        const auto bytes = fixture(file);
        QVERIFY(!bytes.isEmpty());
        SaveDocument doc;
        QString error;
        QVERIFY2(SaveDocument::parse(bytes, doc, error), qPrintable(error));
        QCOMPARE(doc.characters().size(), count);
        QCOMPARE(doc.serialize(), bytes);

        const auto original = doc;
        doc.editCharacter(0).character["memory"] = 123456;
        SaveDocument edited;
        QVERIFY2(SaveDocument::parse(doc.serialize(), edited, error), qPrintable(error));
        auto expected = original.characters()[0].character;
        expected["memory"] = 123456;
        QCOMPARE(edited.characters()[0].character, expected);
        QCOMPARE(edited.characters()[0].info, original.characters()[0].info);
        QCOMPARE(edited.characters()[0].quests, original.characters()[0].quests);
        QCOMPARE(edited.settings(), original.settings());
        if (count == 2) {
            QCOMPARE(edited.characters()[1].character, original.characters()[1].character);
            QCOMPARE(edited.characters()[1].info, original.characters()[1].info);
            QCOMPARE(edited.characters()[1].quests, original.characters()[1].quests);
            QCOMPARE(doc.serialize().mid(doc.serialize().indexOf(marker)),
                     bytes.mid(bytes.indexOf(marker)));
        }
        // Only the first character object changes; all bytes after it stay unchanged.
        const auto separator = bytes.indexOf(";{");
        const auto updated = doc.serialize();
        QVERIFY(separator > 0);
        QCOMPARE(updated.mid(updated.indexOf(";{")), bytes.mid(separator));
    }

    void stringDelimiters()
    {
        const QByteArray bytes = QByteArray("{\"setting\":\"~SettingsSplit~\"}~SettingsSplit~") + slot + marker + slot;
        SaveDocument doc;
        QString error;
        QVERIFY2(SaveDocument::parse(bytes, doc, error), qPrintable(error));
        QCOMPARE(doc.characters().size(), 2);
        QCOMPARE(doc.serialize(), bytes);
        doc.editCharacter(0).character["memory"] = 10;
        SaveDocument reloaded;
        QVERIFY2(SaveDocument::parse(doc.serialize(), reloaded, error), qPrintable(error));
        QCOMPARE(reloaded.characters()[0].character["unknown"], doc.characters()[1].character["unknown"]);
        QVERIFY(doc.serialize().endsWith(marker + slot));
    }

    void rejectsInvalid_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("bad settings") << QByteArray("{oops}~SettingsSplit~");
        QTest::newRow("wrong marker") << QByteArray("{}~Other~");
        QTest::newRow("bad character") << QByteArray("{}~SettingsSplit~[];{};{};misc");
        QTest::newRow("bad info") << QByteArray("{}~SettingsSplit~{};{no};{};misc");
        QTest::newRow("bad quest") << QByteArray("{}~SettingsSplit~{};{};[];misc");
        QTest::newRow("no misc") << QByteArray("{}~SettingsSplit~{};{};{}; ");
        QTest::newRow("malformed misc object") << QByteArray("{}~SettingsSplit~{};{};{};{oops}");
        QTest::newRow("duplicate settings marker") << QByteArray("{}~SettingsSplit~{};{};{};misc~SettingsSplit~");
        QTest::newRow("trailing character marker") << (QByteArray("{}~SettingsSplit~") + slot + marker);
        QTest::newRow("partial second character") << (QByteArray("{}~SettingsSplit~") + slot + marker + "{};{};");
    }
    void rejectsInvalid()
    {
        QFETCH(QByteArray, bytes);
        SaveDocument doc;
        QString error;
        const auto good = fixture("SaveData1.txt");
        QVERIFY(SaveDocument::parse(good, doc, error));
        QVERIFY(!SaveDocument::parse(bytes, doc, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(doc.serialize(), good);
    }

    void missingEmptyAndNoCharacters()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SaveStore store{SavePaths(dir.path())};
        QString error;
        QVERIFY(!store.load(error));
        QVERIFY(error.contains("Cannot open"));
        QVERIFY(!store.canEdit());
        QVERIFY(put(store.paths().primary(), {}));
        QVERIFY(!store.load(error));
        QVERIFY(!error.isEmpty());
        QVERIFY(put(store.paths().primary(), "{}~SettingsSplit~"));
        QVERIFY(store.load(error));
        QVERIFY(!store.canEdit());
        QVERIFY(store.document().characters().isEmpty());
        QVERIFY(!store.mutateCharacter(0, [](SaveCharacter &) {}, error));
    }

    void transaction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SaveStore store{SavePaths(dir.path())};
        const auto original = fixture("SaveData.txt");
        const auto path = store.paths().primary();
        QVERIFY(put(path, original));
        QVERIFY(put(store.paths().gameBackup(), "game"));
        QVERIFY(put(store.paths().legacyEditorBackup(), "legacy"));
        QString error;
        QVERIFY2(store.load(error), qPrintable(error));
        QVERIFY2(store.mutateCharacter(1, [](SaveCharacter &s) {
            s.character["memory"] = 999;
            s.info["bossFightAssistances"] = 0;
            s.quests["questStage_Hoover"] = 10;
        }, error), qPrintable(error));
        QVERIFY(read(path) != original);
        QCOMPARE(read(path), store.document().serialize());
        QCOMPARE(store.document().characters()[1].character["memory"].toInt(), 999);
        QCOMPARE(store.document().characters()[1].quests["questStage_Hoover"].toInt(), 10);
        QCOMPARE(read(store.paths().gameBackup()), QByteArray("game"));
        QCOMPARE(read(store.paths().legacyEditorBackup()), QByteArray("legacy"));
        // Second edit uses the newly committed bytes as its expected baseline.
        QVERIFY2(store.mutateCharacter(0, [](SaveCharacter &s) { s.character["memory"] = 8; }, error), qPrintable(error));
        QCOMPARE(store.document().characters()[1].character["memory"].toInt(), 999);
        SaveStore reopened{SavePaths(dir.path())};
        QVERIFY2(reopened.load(error), qPrintable(error));
        QCOMPARE(reopened.document().serialize(), store.document().serialize());
    }

    void failuresKeepState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SaveStore store{SavePaths(dir.path())};
        const auto path = store.paths().primary();
        const auto original = fixture("SaveData1.txt");
        QVERIFY(put(path, original));
        QString error;
        QVERIFY(store.load(error));
        QVERIFY(!QFile::exists(store.paths().legacyEditorBackup()));
        auto mutation = [](SaveCharacter &s) { s.character["memory"] = 100; };
        QVERIFY(!store.mutateCharacter(-1, mutation, error));
        QLockFile other(path + ".editor.lock");
        QVERIFY(other.tryLock());
        QVERIFY(!store.mutateCharacter(0, mutation, error));
        QCOMPARE(read(path), original);
        QCOMPARE(store.document().serialize(), original);
        other.unlock();

        // Simulate a concurrent game write after the initial check, but before
        // commit: staging must be canceled and the external bytes must survive.
        const QByteArray external = original + "\n";
        bool externalWritten = false;
        QVERIFY(!store.mutateCharacter(0, [&](SaveCharacter &s) {
            mutation(s);
            externalWritten = put(path, external);
        }, error));
        QVERIFY(externalWritten);
        QVERIFY(error.contains("changed externally"));
        QCOMPARE(read(path), external);
        QCOMPARE(store.document().serialize(), original);
        QVERIFY(!store.mutateCharacter(0, mutation, error));
        QCOMPARE(read(path), external);
        QVERIFY(put(path, "invalid"));
        QVERIFY(!store.load(error));
        QVERIFY(!store.canEdit());
        QCOMPARE(store.document().serialize(), original);
        QVERIFY(!store.mutateCharacter(0, mutation, error));
        QCOMPARE(read(path), QByteArray("invalid"));
    }

    void readOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SaveStore store{SavePaths(dir.path())};
        const auto path = store.paths().primary();
        const auto original = fixture("SaveData1.txt");
        QVERIFY(put(path, original));
        const auto originalPermissions = QFile::permissions(path);
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        if (QFileInfo(path).isWritable()) {
            QFile::setPermissions(path, originalPermissions);
            QSKIP("Filesystem does not enforce read-only permissions for this user.");
        }
        QString error;
        QVERIFY(store.load(error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.canEdit());
        QVERIFY(!store.mutateCharacter(0, [](SaveCharacter &s) { s.character["memory"] = 7; }, error));
        QCOMPARE(read(path), original);
        QCOMPARE(store.document().serialize(), original);
        QVERIFY(QFile::setPermissions(path, originalPermissions));
    }
};

QTEST_GUILESS_MAIN(SaveCoreTests)
#include "savecore_tests.moc"
