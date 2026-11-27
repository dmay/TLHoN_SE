#include "jsonparser.h"
#include "mainwindow.h"
#include "tab_settings.h"
#include "tab_stats.h"
#include <QCheckBox>
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
};

QTEST_MAIN(EditorTests)
#include "editor_tests.moc"
