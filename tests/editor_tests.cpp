#include "jsonparser.h"
#include "backupservice.h"
#include "backupmanager.h"
#include "restoreservice.h"
#include "mainwindow.h"
#include "tab_settings.h"
#include "tab_stats.h"
#include "tab_inventory.h"
#include <QJsonDocument>
#include <QUuid>
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
        const QByteArray baseline = R"({}~SettingsSplit~{"name":"First","memory":11};{};{};opaque;~NoticeMeSenpaiNoticeMe~{"name":"Second","memory":22,"maxIconUses":4,"inventory":[{"GUID":"unrelated","instanceIDOnCreation":"existing","uses":1,"isAwake":false,"hasSeen":true,"isHoovered":false,"newGamePlusCount":0}],"inventoryWeapons":[]};{"bossFightAssistances":3};{"questStage_Main":7};opaque;)";
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

    void catalogGrantUi()
    {
        QTemporaryDir dir, prefs;
        QVERIFY(dir.isValid() && prefs.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
        QSettings("DeathWhisper", "TLHoN_SE").setValue("Cheat mode", "1");
        JsonParser::configure(SavePaths(dir.path()));
        const auto paths = JsonParser::paths();
        const auto put = [](const QString &path, const QByteArray &bytes) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.flush();
        };
        const auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        QString error;
        SaveDocument original;
        QVERIFY2(SaveDocument::parse(read(QDir(FIXTURE_DIR).filePath("SaveData.txt")), original, error), qPrintable(error));
        auto &second = original.editCharacter(1);
        auto generic = second.character.value("inventory").toArray();
        const QString ownedGuid = "MasterChefArmor_Helm";
        QJsonObject duplicate;
        int alreadyOwned = 0;
        for (const auto &value : generic) {
            if (value.toObject().value("GUID").toString() == ownedGuid) {
                duplicate = value.toObject();
                ++alreadyOwned;
            }
        }
        QVERIFY(!duplicate.isEmpty());
        duplicate["instanceIDOnCreation"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        generic.append(duplicate);
        ++alreadyOwned;
        second.character["inventory"] = generic;
        const auto baseline = original.serialize();
        QVERIFY(put(paths.primary(), baseline));
        QVERIFY(put(paths.gameBackup(), "game companion"));
        QVERIFY(put(paths.legacyEditorBackup(), "legacy editor backup"));
        QVERIFY2(JsonParser::init(error), qPrintable(error));

        // Entirely synthetic paths/allow-list, not a qualification of real items.
        const QString catalogPath = dir.filePath("catalog.json"), reviewPath = dir.filePath("review.json");
        const QJsonArray ids{
            "Assets/Design/GameplayData/Items/Armour/MasterChefArmor_Helm.asset",
            "Assets/Design/GameplayData/Items/Armour/TestArmor.asset",
            "Assets/Design/GameplayData/Items/Armour/testArmor.asset",
            "Assets/Design/GameplayData/Items/Armour/DLCArmours/TestDlcArmor.asset",
            "Assets/Design/GameplayData/Items/Weapons/Swords/TestSword.asset",
            "Assets/Design/GameplayData/Items/Weapons/Swords/ExtraSword.asset",
            "Assets/Design/GameplayData/Items/Shields/TestShield.asset",
            "Assets/Design/GameplayData/Items/Weapons/Swords/UnreviewedSword.asset",
            "Assets/Design/GameplayData/Items/Weapons/BaseTypes/BaseSword.asset"
        };
        const QByteArray catalogBytes = QJsonDocument(QJsonObject{{"m_InternalIds", ids}}).toJson();
        QVERIFY(put(catalogPath, catalogBytes));
        ItemCatalog::Source source{catalogPath, "synthetic"};
        ItemCatalog::Catalog catalog;
        QVERIFY2(ItemCatalog::load(source, catalog, error), qPrintable(error));
        QJsonArray reviewed;
        for (const auto &guid : {"MasterChefArmor_Helm", "TestArmor", "testArmor", "TestDlcArmor", "TestSword", "ExtraSword", "TestShield"}) {
            const auto *entry = catalog.find(guid);
            QVERIFY(entry);
            QJsonObject row{{"guid", entry->guid}, {"sourcePath", entry->sourcePaths.first()},
                {"status", "allow"}, {"category", ItemCatalog::categoryName(entry->category)},
                {"dlc", entry->dlc == ItemCatalog::Dlc::DlcPath ? "dlc" : "base"}, {"evidence", "Synthetic test review"}};
            if (entry->category == ItemCatalog::Category::Armor) row["slot"] = "helm";
            if (entry->guid == "TestArmor") row["displayName"] = "Sample Helmet";
            reviewed.append(row);
        }
        const auto reviewBytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"catalogSha256", catalog.sha256}, {"entries", reviewed}}).toJson();
        QVERIFY(put(reviewPath, reviewBytes));

        MainWindow window;
        window.show();
        auto *inventory = window.findChild<Tab_Inventory *>();
        auto *settings = window.findChild<Tab_Settings *>();
        auto *stats = window.findChild<Tab_Stats *>();
        QVERIFY(inventory && settings && stats);
        auto *character = settings->findChild<QComboBox *>("cb_saves");
        auto *table = inventory->findChild<QTableWidget *>("catalogTable");
        auto *search = inventory->findChild<QLineEdit *>("catalogSearch");
        auto *filter = inventory->findChild<QComboBox *>("categoryFilter");
        auto *onlyMissing = inventory->findChild<QCheckBox *>("missingOnly");
        auto *entitled = inventory->findChild<QCheckBox *>("dlcEntitled");
        auto *qualified = inventory->findChild<QCheckBox *>("bulkQualified");
        auto *preview = inventory->findChild<QPlainTextEdit *>("itemPreview");
        auto *add = inventory->findChild<QPushButton *>("addSelected");
        auto *allArmor = stats->findChild<QPushButton *>("btn_all_armor");
        auto *allWeapons = stats->findChild<QPushButton *>("btn_all_weapon");
        auto *cheats = stats->findChild<QPushButton *>("btn_cheat");
        auto *openInventory = window.findChild<QPushButton *>("btn_inventory");
        auto *openStats = window.findChild<QPushButton *>("btn_stats");
        QVERIFY(character && table && search && filter && onlyMissing && entitled && qualified && preview && add && allArmor && allWeapons && cheats && openInventory && openStats);
        character->setCurrentIndex(1);
        openInventory->click();
        QVERIFY(inventory->isVisible());
        QVERIFY(!add->isEnabled());
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!inventory->loadCatalog({dir.filePath("missing.json"), {}}));
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(inventory->loadCatalog(source));
        QVERIFY(table->rowCount() > 0);
        QVERIFY(!qualified->isEnabled());
        const auto select = [&](const QString &guid) {
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 0)->data(Qt::UserRole).toString() == guid) {
                    table->selectRow(row);
                    return row;
                }
            }
            return -1;
        };
        QVERIFY(select("TestArmor") >= 0);
        QVERIFY(!add->isEnabled());
        QVERIFY(inventory->loadCatalog(source, reviewPath));
        QVERIFY(!qualified->isChecked() && !entitled->isChecked());
        search->setText("Sample Helmet");
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(select("TestArmor") >= 0);
        QVERIFY(add->isEnabled());
        QVERIFY(preview->toPlainText().contains("inventory"));
        search->setText("testarmor");
        QCOMPARE(table->rowCount(), 2); // case-insensitive search; exact identity below
        search->clear();
        filter->setCurrentIndex(filter->findData(int(ItemCatalog::Category::Weapon)));
        QCOMPARE(table->rowCount(), 4);
        QVERIFY(select("UnreviewedSword") >= 0);
        QVERIFY(!add->isEnabled());
        QVERIFY(select("BaseSword") >= 0);
        QVERIFY(!add->isEnabled());
        filter->setCurrentIndex(0);
        int ownedRow = select(ownedGuid);
        QVERIFY(ownedRow >= 0);
        QCOMPARE(table->item(ownedRow, 4)->text(), QString::number(alreadyOwned));
        QVERIFY(!add->isEnabled());
        onlyMissing->setChecked(true);
        QCOMPARE(select(ownedGuid), -1);
        onlyMissing->setChecked(false);
        QVERIFY(select("TestDlcArmor") >= 0);
        QVERIFY(!add->isEnabled());
        entitled->setChecked(true);
        QVERIFY(add->isEnabled());
        entitled->setChecked(false);
        QVERIFY(!add->isEnabled());
        QCOMPARE(read(paths.primary()), baseline);

        // Respond to confirmation and any subsequent result modal. Queue
        // button clicks outside the polling callback (nested modal loops).
        struct Dialogs { QStringList questions, infos, warnings; };
        auto press = [&](QPushButton *button, bool accept, const std::function<void()> &during = {}) {
            Dialogs result;
            QTimer timer;
            connect(&timer, &QTimer::timeout, &window, [&] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box || box->property("testHandled").toBool()) return;
                box->setProperty("testHandled", true);
                auto answer = QMessageBox::Ok;
                if (box->icon() == QMessageBox::Question) {
                    result.questions.append(box->text());
                    answer = accept ? QMessageBox::Yes : QMessageBox::No;
                    if (during) during();
                } else if (box->icon() == QMessageBox::Warning) result.warnings.append(box->text());
                else result.infos.append(box->text());
                if (auto *b = box->button(answer)) QTimer::singleShot(0, b, [b] { b->click(); });
            });
            timer.start(5);
            button->click();
            timer.stop();
            return result;
        };
        openStats->click();
        cheats->setChecked(true);
        QVERIFY(allArmor->isVisible() && allWeapons->isVisible());
        // Qualification is an explicit gate; not inferred from a reviewed path.
        auto gated = press(allArmor, true);
        QCOMPARE(gated.warnings.size(), 1);
        QVERIFY(gated.questions.isEmpty());
        QCOMPARE(read(paths.primary()), baseline);
        openInventory->click();
        qualified->setChecked(true); // synthetic fixture qualification only
        QVERIFY(select("TestArmor") >= 0);
        const auto canceled = press(add, false);
        QCOMPARE(canceled.questions.size(), 1);
        QVERIFY(canceled.questions.first().contains("Add: 1"));
        QCOMPARE(read(paths.primary()), baseline);
        const auto armor = press(add, true);
        QCOMPARE(armor.infos.size(), 1);
        QVERIFY(armor.infos.first().contains("Items added: 1"));
        QVERIFY(!add->isEnabled());
        QVERIFY(select("TestSword") >= 0);
        QVERIFY(preview->toPlainText().contains("inventoryWeapons"));
        const auto sword = press(add, true);
        QCOMPARE(sword.infos.size(), 1);
        QVERIFY(sword.infos.first().contains("Items added: 1"));

        openStats->click();
        const auto bulkArmor = press(allArmor, true);
        QCOMPARE(bulkArmor.questions.size(), 1);
        QVERIFY(bulkArmor.questions.first().contains("Add: 1; owned: 2; ineligible: 1"));
        QVERIFY(bulkArmor.infos.first().contains("Items added: 1"));
        const auto bulkWeapons = press(allWeapons, true);
        QCOMPARE(bulkWeapons.questions.size(), 1);
        QVERIFY(bulkWeapons.questions.first().contains("Add: 2; owned: 1; ineligible: 2"));
        QVERIFY(bulkWeapons.infos.first().contains("Items added: 2"));
        const auto after = read(paths.primary());
        const auto time = QFileInfo(paths.primary()).lastModified();
        const auto repeatedArmor = press(allArmor, true);
        QCOMPARE(repeatedArmor.infos.size(), 1);
        QVERIFY(repeatedArmor.infos.first().contains("New items: 0"));
        QCOMPARE(press(allWeapons, true).questions.size(), 0);
        QCOMPARE(read(paths.primary()), after);
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), time);

        SaveDocument disk;
        QVERIFY2(SaveDocument::parse(after, disk, error), qPrintable(error));
        QCOMPARE(disk.characters()[0].character, original.characters()[0].character);
        const auto &updated = disk.characters()[1];
        QCOMPARE(updated.character.value("inventory").toArray().size(), generic.size() + 2);
        QCOMPARE(updated.character.value("inventoryWeapons").toArray().size(), second.character.value("inventoryWeapons").toArray().size() + 3);
        QList<Inventory::Instance> instances;
        QVERIFY2(Inventory::read(updated, instances, error), qPrintable(error));
        for (const auto &item : instances) QVERIFY(item.guid != "TestDlcArmor" && item.guid != "UnreviewedSword" && item.guid != "BaseSword");
        SaveDocument expected;
        QVERIFY(SaveDocument::parse(baseline, expected, error));
        expected.editCharacter(1).character["inventory"] = updated.character.value("inventory");
        expected.editCharacter(1).character["inventoryWeapons"] = updated.character.value("inventoryWeapons");
        QCOMPARE(after, expected.serialize()); // all unrelated bytes, including other sections
        for (qsizetype i = 0; i < generic.size(); ++i) QCOMPARE(updated.character.value("inventory").toArray()[i], generic[i]);
        QCOMPARE(read(paths.gameBackup()), QByteArray("game companion"));
        QCOMPARE(read(paths.legacyEditorBackup()), QByteArray("legacy editor backup"));

        // Switching characters changes the target, not the catalog selection.
        character->setCurrentIndex(0);
        openInventory->click();
        QVERIFY(select("TestSword") >= 0 && add->isEnabled());
        const auto switchedDuringConfirm = press(add, true, [&] { character->setCurrentIndex(1); });
        QCOMPARE(switchedDuringConfirm.warnings.size(), 1);
        QCOMPARE(read(paths.primary()), after);
        character->setCurrentIndex(0);
        QVERIFY(select("TestSword") >= 0 && add->isEnabled());
        QCOMPARE(press(add, true).infos.size(), 1);
        SaveDocument afterSwitch;
        QVERIFY(SaveDocument::parse(read(paths.primary()), afterSwitch, error));
        QCOMPARE(afterSwitch.characters()[1].character, updated.character);
        QCOMPARE(afterSwitch.characters()[0].character.value("inventoryWeapons").toArray().size(), original.characters()[0].character.value("inventoryWeapons").toArray().size()+1);
        const auto afterSwitchBytes = read(paths.primary());
        settings->reloadSave();
        QVERIFY(select("TestSword") >= 0 && !add->isEnabled());
        QCOMPARE(read(paths.primary()), afterSwitchBytes);

        // External writer during confirmation must fail closed.
        QVERIFY(select("TestArmor") >= 0 && add->isEnabled());
        afterSwitch.editCharacter(0).character["memory"] = 1234;
        const auto external = afterSwitch.serialize();
        bool wrote = false;
        const auto stale = press(add, true, [&] { wrote = put(paths.primary(), external); });
        QVERIFY(wrote);
        QCOMPARE(stale.warnings.size(), 1);
        QCOMPARE(read(paths.primary()), external);
        settings->reloadSave();
        QVERIFY(select("TestArmor") >= 0 && add->isEnabled());
        const auto newCatalog = press(add, true, [&] { wrote = put(catalogPath, catalogBytes + "\n"); });
        QVERIFY(wrote);
        QCOMPARE(newCatalog.warnings.size(), 1);
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!add->isEnabled() && !qualified->isChecked());
        QCOMPARE(read(paths.primary()), external);
        // Missing/corrupt save disables grants; no write on reload or open.
        QVERIFY(QFile::remove(paths.primary()));
        settings->reloadSave();
        QVERIFY(!add->isEnabled() && !allArmor->isEnabled());
        QVERIFY(!QFile::exists(paths.primary()));
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
