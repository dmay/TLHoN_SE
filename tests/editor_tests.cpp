#include "jsonparser.h"
#include "backupservice.h"
#include "backupmanager.h"
#include "restoreservice.h"
#include "mainwindow.h"
#include "tab_settings.h"
#include "tab_stats.h"
#include <QCheckBox>
#include <QComboBox>
#include <QInputDialog>
#include <QApplication>
#include <QAbstractButton>
#include <QMessageBox>
#include <QTimer>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QLabel>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class EditorTests : public QObject
{
    Q_OBJECT
private slots:
    void startupAndEdits()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir settings;
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        JsonParser::configure(SavePaths(dir.path()));
        QString error;
        QVERIFY(!JsonParser::init(error));
        MainWindow window;
        window.setSaveStatus(error);
        auto *settingsWidget = window.findChild<Tab_Settings *>();
        auto *stats = window.findChild<Tab_Stats *>();
        QVERIFY(settingsWidget);
        QVERIFY(stats);
        auto *icons = settingsWidget->findChild<QLineEdit *>("le_maxIconUses");
        auto *memory = stats->findChild<QLineEdit *>("le_memory");
        auto *recovery = settingsWidget->findChild<QPushButton *>("btn_restore");
        QVERIFY(icons && memory && recovery);
        QVERIFY(!icons->isEnabled());
        QVERIFY(!memory->isEnabled());
        QVERIFY(recovery->isEnabled());

        // Manual recovery is represented by copying a fixture into the
        // disposable directory, then invoking the same reload/refresh hooks.
        const QString path = JsonParser::paths().primary();
        QVERIFY(QFile::copy(QDir(FIXTURE_DIR).filePath("SaveData.txt"), path));
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        settingsWidget->loadGameSaves();
        window.setSaveStatus(error);
        QVERIFY(icons->isEnabled());
        QVERIFY(memory->isEnabled());

        icons->setText("9");
        QVERIFY(QMetaObject::invokeMethod(icons, "editingFinished"));
        memory->setText("4567");
        QVERIFY(QMetaObject::invokeMethod(memory, "editingFinished"));
        auto *backstabs = stats->findChild<QCheckBox *>("cb_backstabs");
        QVERIFY(backstabs);
        if (backstabs->isEnabled()) backstabs->setChecked(true);
        SaveStore reloaded{SavePaths(dir.path())};
        QVERIFY2(reloaded.load(error), qPrintable(error));
        QCOMPARE(reloaded.document().characters()[0].character["maxIconUses"].toInt(), 9);
        QCOMPARE(reloaded.document().characters()[0].character["memory"].toInt(), 4567);
        QCOMPARE(reloaded.document().characters()[0].character["backstabCount"],
                 JsonParser::characters()[0].character["backstabCount"]);
        QVERIFY(!QFile::exists(JsonParser::paths().legacyEditorBackup()));
    }

    void statsAndNoOpRefresh()
    {
        QTemporaryDir saves;
        QTemporaryDir prefs;
        QVERIFY(saves.isValid() && prefs.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
        JsonParser::configure(SavePaths(saves.path()));
        const auto paths = JsonParser::paths();
        const QByteArray baseline = R"({}~SettingsSplit~{"name":"First","memory":11};{};{};opaque;~NoticeMeSenpaiNoticeMe~{"name":"Second","memory":22,"maxIconUses":4,"inventory":[{"GUID":"unrelated"}]};{"bossFightAssistances":3};{"questStage_Main":7};opaque;)";
        const auto put = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
        };
        const auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QVERIFY(put(paths.primary(), baseline));
        QVERIFY(put(paths.gameBackup(), "companion"));
        QVERIFY(put(paths.legacyEditorBackup(), "legacy"));
        QString error;
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        MainWindow window;
        auto *tab = window.findChild<Tab_Settings *>();
        auto *stats = window.findChild<Tab_Stats *>();
        QVERIFY(tab && stats);
        auto *selector = tab->findChild<QComboBox *>("cb_saves");
        auto *memory = stats->findChild<QLineEdit *>("le_memory");
        auto *icons = tab->findChild<QLineEdit *>("le_maxIconUses");
        QVERIFY(selector && memory && icons);
        QCOMPARE(selector->count(), 2);
        selector->setCurrentIndex(1);
        QCOMPARE(memory->text(), QString("22"));

        // A byte-equal rewrite would not be caught by a hash alone. Set a
        // sentinel mtime, then confirm that even the timestamp is unchanged.
        const auto sentinel = QDateTime::fromSecsSinceEpoch(1577836800, Qt::UTC);
        for (const auto &path : {paths.primary(), paths.gameBackup(), paths.legacyEditorBackup()}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(sentinel, QFileDevice::FileModificationTime));
        }
        const auto primaryTime = QFileInfo(paths.primary()).lastModified();
        const auto companionTime = QFileInfo(paths.gameBackup()).lastModified();
        const auto legacyTime = QFileInfo(paths.legacyEditorBackup()).lastModified();
        selector->setCurrentIndex(0);
        QCOMPARE(memory->text(), QString("11"));
        selector->setCurrentIndex(1);
        tab->reloadSave();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(memory->text(), QString("22"));
        QVERIFY(QMetaObject::invokeMethod(memory, "editingFinished"));
        QVERIFY(QMetaObject::invokeMethod(icons, "editingFinished"));
        QCOMPARE(read(paths.primary()), baseline);
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), primaryTime);
        QCOMPARE(QFileInfo(paths.gameBackup()).lastModified(), companionTime);
        QCOMPARE(QFileInfo(paths.legacyEditorBackup()).lastModified(), legacyTime);

        const QMap<QString, QString> fields = {
            {"le_memory", "memory"}, {"le_stat_vitality", "_stat_vitality"},
            {"le_stat_stamina", "_stat_stamina"}, {"le_stat_equipLoad", "_stat_equipLoad"},
            {"le_stat_strength", "_stat_strength"}, {"le_stat_dexterity", "_stat_dexterity"},
            {"le_stat_attunement", "_stat_attunement"}, {"le_stat_luck", "_stat_luck"}
        };
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            auto *line = stats->findChild<QLineEdit *>(it.key());
            QVERIFY(line && line->isEnabled());
            line->setText("31");
            QVERIFY(QMetaObject::invokeMethod(line, "editingFinished"));
        }
        icons->setText("9");
        QVERIFY(QMetaObject::invokeMethod(icons, "editingFinished"));
        const QMap<QString, QPair<QString, int>> achievements = {
            {"cb_backstabs", {"backstabCount", 20}}, {"cb_repostes", {"reposteCount", 20}},
            {"cb_rebuffs", {"rebuffCount", 20}}, {"cb_remembered", {"itemsRemembered", 75}},
            {"cb_unburdened", {"itemsHoovered", 71}}, {"cb_DLC", {"DLCItemsRemembered", 10}}
        };
        for (auto it = achievements.cbegin(); it != achievements.cend(); ++it) {
            auto *box = stats->findChild<QCheckBox *>(it.key());
            QVERIFY(box && box->isEnabled());
            box->setChecked(true);
        }
        for (const auto *name : {"cb_help", "cb_restore_quests", "cb_all_spell", "cb_all_rune"}) {
            auto *box = stats->findChild<QCheckBox *>(name);
            QVERIFY(box && box->isEnabled());
            box->setChecked(true);
        }
        SaveStore disk(paths);
        QVERIFY2(disk.load(error), qPrintable(error));
        QCOMPARE(disk.document().characters().size(), 2);
        const auto &slot = disk.document().characters()[1];
        for (const auto &field : fields) QCOMPARE(slot.character[field].toInt(), 31);
        for (const auto &achievement : achievements)
            QCOMPARE(slot.character[achievement.first].toInt(), achievement.second);
        QCOMPARE(slot.character["maxIconUses"].toInt(), 9);
        QCOMPARE(slot.info["bossFightAssistances"].toInt(), 0);
        QCOMPARE(slot.quests["questStage_Main"].toInt(), 7);
        QCOMPARE(slot.quests["questStage_Hoover"].toInt(), 10);
        const auto inventory = slot.character["inventory"].toArray();
        QCOMPARE(inventory.size(), 43); // existing entry + 17 spells + 25 runes
        QCOMPARE(inventory[0].toObject()["GUID"].toString(), QString("unrelated"));
        SaveDocument original;
        QVERIFY(SaveDocument::parse(baseline, original, error));
        QCOMPARE(disk.document().characters()[0].character, original.characters()[0].character);
        QCOMPARE(disk.document().characters()[0].info, original.characters()[0].info);
        QCOMPARE(disk.document().characters()[0].quests, original.characters()[0].quests);
        QCOMPARE(read(paths.gameBackup()), QByteArray("companion"));
        QCOMPARE(read(paths.legacyEditorBackup()), QByteArray("legacy"));
        QCOMPARE(QFileInfo(paths.gameBackup()).lastModified(), companionTime);
        QCOMPARE(QFileInfo(paths.legacyEditorBackup()).lastModified(), legacyTime);

        // Simulate an external writer, wait for availability polling, and
        // confirm the stale controls cannot replace the new disk bytes.
        auto external = disk.document();
        external.editCharacter(1).character["memory"] = 77;
        const auto externalBytes = external.serialize();
        QVERIFY(put(paths.primary(), externalBytes));
        QTRY_VERIFY(!memory->isEnabled() && !icons->isEnabled());
        memory->setText("99");
        QVERIFY(QMetaObject::invokeMethod(memory, "editingFinished"));
        QCOMPARE(read(paths.primary()), externalBytes);
        tab->reloadSave();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(memory->text(), QString("77"));
        QVERIFY(memory->isEnabled() && icons->isEnabled());
        QCOMPARE(read(paths.primary()), externalBytes);
    }

    void stubbedCatalog()
    {
        class Stub final : public BackupManagerBackend
        {
        public:
            bool closed = true;
            bool valid = true;
            bool pending = false;
            bool listFails = false;
            bool createFails = true;
            int restoreCalls = 0;
            int removeCalls = 0;
            QString storageDirectory() const override { return "backups"; }
            QString saveDirectory() const override { return "saves"; }
            bool list(QList<BackupCatalogEntry> &entries, QString &error) override {
                if (listFails) { error = "test storage error"; return false; }
                BackupSnapshot normal;
                normal.id = "normal"; normal.valid = true; normal.label = "Named";
                BackupSnapshot corrupt = normal;
                corrupt.id = "corrupt"; corrupt.valid = false;
                BackupSnapshot safety = normal;
                safety.id = "safety"; safety.kind = BackupKind::Safety;
                entries = {{normal, true, {}}, {corrupt, false, "checksum mismatch"},
                           {safety, false, "missing primary"}};
                return true;
            }
            bool activeValid(QString &error) override { error = valid ? QString() : "bad active"; return valid; }
            bool gameClosed(QString &error) override { error = closed ? QString() : "running"; return closed; }
            bool pendingRecovery() const override { return pending; }
            bool legacyAvailable() const override { return true; }
            bool create(const QString &, bool, BackupSnapshot &, QString &error) override {
                error = "test capture failure"; return !createFails;
            }
            bool rename(const QString &, const QString &, QString &) override { return true; }
            bool remove(const QString &, QString &) override { ++removeCalls; return true; }
            bool chooseStorage(const QString &, QString &) override { return true; }
            QString restore(const QString &, QWidget *) override { ++restoreCalls; return "test restore"; }
            QString reload() override { return "test reload"; }
        } stub;
        BackupManager manager(stub);
        auto *table = manager.findChild<QTableWidget *>("snapshotTable");
        auto *create = manager.findChild<QPushButton *>("createBackup");
        auto *restore = manager.findChild<QPushButton *>("restoreSnapshot");
        auto *remove = manager.findChild<QPushButton *>("deleteSnapshot");
        auto *rename = manager.findChild<QPushButton *>("editLabel");
        auto *refresh = manager.findChild<QPushButton *>("refreshBackups");
        auto *details = manager.findChild<QPlainTextEdit *>("snapshotDetails");
        auto *status = manager.findChild<QLabel *>("backupStatus");
        QVERIFY(table && create && restore && remove && rename && refresh && details && status);
        QCOMPARE(table->rowCount(), 3);
        table->selectRow(1);
        QVERIFY(!restore->isEnabled() && !rename->isEnabled() && !remove->isEnabled());
        QVERIFY(details->toPlainText().contains("checksum mismatch"));
        table->selectRow(2);
        QVERIFY(!restore->isEnabled() && !rename->isEnabled() && !remove->isEnabled());
        table->selectRow(0);
        QVERIFY(restore->isEnabled() && create->isEnabled() && rename->isEnabled() && remove->isEnabled());
        QTimer::singleShot(0, &manager, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box) box->button(QMessageBox::No)->click();
        });
        remove->click();
        QCOMPARE(stub.removeCalls, 0);
        QTimer::singleShot(0, &manager, [] {
            auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (input) input->accept();
        });
        create->click();
        QVERIFY(status->text().contains("test capture failure"));
        stub.closed = false;
        QTRY_VERIFY(!create->isEnabled() && !restore->isEnabled() && !remove->isEnabled());
        stub.closed = true;
        stub.valid = false;
        QTRY_VERIFY(!create->isEnabled() && !remove->isEnabled() && restore->isEnabled());
        restore->click();
        QCOMPARE(stub.restoreCalls, 1);
        stub.pending = true;
        refresh->click();
        QVERIFY(!restore->isEnabled());
        stub.listFails = true;
        refresh->click();
        QVERIFY(status->text().contains("test storage error"));
        QCOMPARE(table->rowCount(), 0);
    }

#ifdef Q_OS_WIN
    void createSnapshotFromSettings()
    {
        QTemporaryDir saves;
        QTemporaryDir storage;
        QTemporaryDir prefs;
        QVERIFY(saves.isValid() && storage.isValid() && prefs.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
        JsonParser::configure(SavePaths(saves.path()));
        const auto paths = JsonParser::paths();
        QVERIFY(QFile::copy(QDir(FIXTURE_DIR).filePath("SaveData.txt"), paths.primary()));
        QVERIFY(QFile::copy(QDir(FIXTURE_DIR).filePath("SaveData1.txt"), paths.gameBackup()));
        const auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto originalPrimary = read(paths.primary());
        const auto originalCompanion = read(paths.gameBackup());
        QString error;
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QSettings settings("DeathWhisper", "TLHoN_SE");
        QVERIFY2(BackupService::configureStorageDirectory(settings, paths, storage.path(), error), qPrintable(error));
        Tab_Settings tab;
        tab.restoreSettings();
        auto *button = tab.findChild<QPushButton *>("btn_restore");
        QVERIFY(button);
        QCOMPARE(button->text(), QString("Backup Manager"));
        // Queue action clicks after the timer callback returns. A repeating
        // timer cannot re-enter itself while a nested modal exec is running.
        int stage = 0;
        bool namedVisible = false;
        bool renamedVisible = false;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, &tab, [&] {
            auto *active = QApplication::activeModalWidget();
            if (auto *manager = qobject_cast<BackupManager *>(active)) {
                auto *table = manager->findChild<QTableWidget *>("snapshotTable");
                if (stage == 0) {
                    stage = 1;
                    auto *action = manager->findChild<QPushButton *>("createBackup");
                    QTimer::singleShot(0, action, &QPushButton::click);
                } else if (stage == 2) {
                    namedVisible = table->rowCount() == 1 && table->item(0, 1)->text() == "Before boss";
                    stage = 3;
                    auto *action = manager->findChild<QPushButton *>("editLabel");
                    QTimer::singleShot(0, action, &QPushButton::click);
                } else if (stage == 4) {
                    renamedVisible = table->rowCount() == 1 && table->item(0, 1)->text() == "After rename";
                    stage = 5;
                    manager->accept();
                } else if (stage == 5) {
                    renamedVisible = renamedVisible && table->rowCount() == 1 && table->item(0, 1)->text() == "After rename";
                    stage = 6;
                    auto *action = manager->findChild<QPushButton *>("deleteSnapshot");
                    QTimer::singleShot(0, action, &QPushButton::click);
                } else if (stage == 7) {
                    stage = 8;
                    manager->accept();
                } else { manager->reject(); }
            } else if (auto *input = qobject_cast<QInputDialog *>(active)) {
                if (stage == 1 || stage == 3) {
                    input->setTextValue(stage == 1 ? "Before boss" : "After rename");
                    ++stage;
                    input->accept();
                } else input->reject();
            } else if (auto *box = qobject_cast<QMessageBox *>(active)) {
                if (stage == 6) {
                    stage = 7;
                    box->button(QMessageBox::Yes)->click();
                } else box->reject();
            }
        });
        dialogs.start(20);
        button->click();
        dialogs.stop();
        QCOMPARE(stage, 5);
        QVERIFY(namedVisible && renamedVisible);
        BackupService service(paths, storage.path());
        QList<BackupSnapshot> snapshots;
        QVERIFY2(service.list(snapshots, error), qPrintable(error));
        QCOMPARE(snapshots.size(), 1);
        QCOMPARE(snapshots[0].label, QString("After rename"));
        QVERIFY(snapshots[0].valid);
        // A newly opened manager reads the persisted catalog.
        dialogs.start(20);
        button->click();
        dialogs.stop();
        QCOMPARE(stage, 8);
        QVERIFY(renamedVisible);
        QVERIFY(service.list(snapshots, error));
        QVERIFY(snapshots.isEmpty());
        QCOMPARE(read(paths.primary()), originalPrimary);
        QCOMPARE(read(paths.gameBackup()), originalCompanion);
    }
    void restoreSnapshotFromSettings()
    {
        QTemporaryDir saves;
        QTemporaryDir storage;
        QTemporaryDir prefs;
        QVERIFY(saves.isValid() && storage.isValid() && prefs.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
        JsonParser::configure(SavePaths(saves.path()));
        const auto paths = JsonParser::paths();
        const QByteArray a = "{}~SettingsSplit~"
            "{\"name\":\"Alice\",\"memory\":11,\"maxIconUses\":3};{};{};opaque;"
            "~NoticeMeSenpaiNoticeMe~"
            "{\"name\":\"Bob\",\"memory\":22,\"maxIconUses\":4,\"backstabCount\":20};{};{};opaque;";
        auto put = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
        };
        auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        auto b = a;
        b.replace("\"memory\":22", "\"memory\":99");
        b.replace("\"maxIconUses\":4", "\"maxIconUses\":8");
        b.replace("\"backstabCount\":20", "\"backstabCount\":0");
        QVERIFY(put(paths.primary(), a));
        QVERIFY(put(paths.gameBackup(), "A companion"));
        QString error;
        QSettings settings("DeathWhisper", "TLHoN_SE");
        QVERIFY2(BackupService::configureStorageDirectory(settings, paths, storage.path(), error), qPrintable(error));
        BackupService service(paths, storage.path());
        BackupSnapshot chosen;
        QVERIFY2(service.createSnapshot("A", chosen, error), qPrintable(error));
        QVERIFY(put(paths.primary(), b));
        QVERIFY(put(paths.gameBackup(), "B companion"));
        QVERIFY2(JsonParser::init(error), qPrintable(error));

        MainWindow window;
        auto *tab = window.findChild<Tab_Settings *>();
        auto *stats = window.findChild<Tab_Stats *>();
        QVERIFY(tab && stats);
        auto *selector = tab->findChild<QComboBox *>("cb_saves");
        auto *icons = tab->findChild<QLineEdit *>("le_maxIconUses");
        auto *memory = stats->findChild<QLineEdit *>("le_memory");
        auto *restoreButton = tab->findChild<QPushButton *>("btn_restore");
        QVERIFY(selector && icons && memory && restoreButton);
        selector->setCurrentIndex(1);
        QCOMPARE(memory->text(), QString("99"));
        QCOMPARE(icons->text(), QString("8"));

        for (int phase = 0; phase < 4; ++phase) {
            const bool confirm = phase != 0;
            // A missing/corrupt active save must still allow a valid restore.
            if (phase == 2) {
                QVERIFY(QFile::remove(paths.primary()));
                tab->reloadSave();
                QCOMPARE(selector->count(), 0);
                QVERIFY(!memory->isEnabled());
                QVERIFY(memory->text().isEmpty());
            }
            if (phase == 3) {
                QVERIFY(put(paths.primary(), "corrupt"));
                tab->reloadSave();
                QCOMPARE(selector->count(), 0);
            }
            int stage = 0;
            bool scopeShown = false;
            bool successShown = false;
            bool enabledForRecovery = false;
            QTimer dialogs;
            connect(&dialogs, &QTimer::timeout, tab, [&] {
                auto *active = QApplication::activeModalWidget();
                if (auto *manager = qobject_cast<BackupManager *>(active)) {
                    if (stage == 0) {
                        auto *table = manager->findChild<QTableWidget *>("snapshotTable");
                        for (int row = 0; row < table->rowCount(); ++row)
                            if (table->item(row, 1)->text() == "A") table->selectRow(row);
                        auto *restore = manager->findChild<QPushButton *>("restoreSnapshot");
                        enabledForRecovery = restore->isEnabled();
                        stage = 1;
                        QTimer::singleShot(0, restore, &QPushButton::click);
                    } else manager->accept();
                } else if (auto *box = qobject_cast<QMessageBox *>(active)) {
                    if (stage == 1 && box->windowTitle() == "Confirm restore") {
                        scopeShown = box->text().contains("ALL characters") &&
                            box->informativeText().contains(paths.directory());
                        stage = 2;
                        box->button(confirm ? QMessageBox::Yes : QMessageBox::No)->click();
                    } else {
                        successShown = box->windowTitle() == "Restore complete";
                        box->accept();
                    }
                }
            });
            dialogs.start(20);
            restoreButton->click();
            dialogs.stop();
            QVERIFY(enabledForRecovery);
            QVERIFY(scopeShown);
            QCOMPARE(successShown, confirm);
            QCOMPARE(read(paths.primary()), confirm ? a : b);
            QCOMPARE(read(paths.gameBackup()), QByteArray(confirm ? "A companion" : "B companion"));
            QList<BackupSnapshot> snapshots;
            QVERIFY(service.list(snapshots, error));
            QCOMPARE(snapshots.size(), phase + 1);
            QCOMPARE(selector->currentIndex(), phase >= 2 ? 0 : 1);
        }
        selector->setCurrentIndex(1);
        QCOMPARE(memory->text(), QString("22"));
        QCOMPARE(icons->text(), QString("4"));
        QCOMPARE(read(paths.primary()), a);
        QVERIFY(!RestoreService::hasPending(paths));
    }
#endif
};

QTEST_MAIN(EditorTests)
#include "editor_tests.moc"
