#include "jsonparser.h"
#include "backupservice.h"
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
#include <QFile>
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
        QString error;
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QSettings settings("DeathWhisper", "TLHoN_SE");
        QVERIFY2(BackupService::configureStorageDirectory(settings, paths, storage.path(), error), qPrintable(error));

        Tab_Settings tab;
        auto *button = tab.findChild<QPushButton *>("btn_restore");
        QVERIFY(button);
        bool actionClicked = false;
        bool successShown = false;
        // Drive the same button a user clicks, without launching the game or
        // touching a real save directory. Reject unknown dialogs to avoid hangs.
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, &tab, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            if (!actionClicked) {
                for (auto *action : box->buttons()) {
                    if (action->text() == "Create backup") {
                        actionClicked = true;
                        action->click();
                        return;
                    }
                }
            } else {
                successShown = box->windowTitle() == "Backup created";
            }
            box->accept();
        });
        dialogs.start(10);
        button->click();
        dialogs.stop();
        QVERIFY(actionClicked);
        QVERIFY(successShown);
        BackupService service(paths, storage.path());
        QList<BackupSnapshot> snapshots;
        QVERIFY2(service.list(snapshots, error), qPrintable(error));
        QCOMPARE(snapshots.size(), 1);
        QVERIFY(snapshots[0].valid);
    }
#ifdef Q_OS_WIN
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

        for (const bool confirm : {false, true}) {
            int stage = 0;
            bool scopeShown = false;
            bool successShown = false;
            QTimer dialogs;
            connect(&dialogs, &QTimer::timeout, tab, [&] {
                auto *active = QApplication::activeModalWidget();
                if (auto *input = qobject_cast<QInputDialog *>(active)) {
                    if (stage == 1) { stage = 2; input->accept(); }
                    else input->reject();
                } else if (auto *box = qobject_cast<QMessageBox *>(active)) {
                    if (stage == 0) {
                        for (auto *action : box->buttons()) {
                            if (action->text() == "Restore snapshot") {
                                stage = 1;
                                action->click();
                                return;
                            }
                        }
                    } else if (stage == 2 && box->windowTitle() == "Confirm restore") {
                        scopeShown = box->text().contains("ALL characters") &&
                            box->informativeText().contains(paths.directory());
                        stage = 3;
                        box->button(confirm ? QMessageBox::Yes : QMessageBox::No)->click();
                        return;
                    } else if (stage == 3) {
                        successShown = box->windowTitle() == "Restore complete";
                    }
                    box->reject();
                }
            });
            dialogs.start(10);
            restoreButton->click();
            dialogs.stop();
            QVERIFY(scopeShown);
            QCOMPARE(successShown, confirm);
            QCOMPARE(read(paths.primary()), confirm ? a : b);
            QCOMPARE(read(paths.gameBackup()), QByteArray(confirm ? "A companion" : "B companion"));
            QList<BackupSnapshot> snapshots;
            QVERIFY(service.list(snapshots, error));
            QCOMPARE(snapshots.size(), confirm ? 2 : 1);
            QCOMPARE(selector->currentIndex(), 1);
        }
        QCOMPARE(memory->text(), QString("22"));
        QCOMPARE(icons->text(), QString("4"));
        QCOMPARE(read(paths.primary()), a);
        QVERIFY(!RestoreService::hasPending(paths));
    }
#endif
};

QTEST_MAIN(EditorTests)
#include "editor_tests.moc"
