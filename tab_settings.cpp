#include "tab_settings.h"
#include "ui_tab_settings.h"

#include "jsonparser.h"
#include "backupservice.h"
#include "stylefromfile.h"
#include "positiveintvalidator.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QUrl>
#include <QSettings>


int Tab_Settings::characterId = -1;
QString Tab_Settings::language = "en";

Tab_Settings::Tab_Settings(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Tab_Settings)
{
    ui->setupUi(this);

    validator = new PositiveIntValidator;
    ui->le_maxIconUses->setValidator(validator);

    setStyleFromFile(this, ":/Resources/StyleSheets/Tab_Settings.qss");
    loadGameSaves();
}

void Tab_Settings::loadGameSaves()
{
    const QSignalBlocker blocker(ui->cb_saves);
    const int previous = characterId;
    ui->cb_saves->clear();
    const auto &characters = JsonParser::characters();
    for (const auto &slot : characters)
        ui->cb_saves->addItem(" " + slot.character["name"].toString());
    const int selection = previous >= 0 && previous < characters.size()
        ? previous : (characters.isEmpty() ? -1 : 0);
    ui->cb_saves->setCurrentIndex(selection);
    ui->cb_saves->setEnabled(!characters.isEmpty());
    on_cb_saves_currentIndexChanged(selection);
}

int Tab_Settings::getCharacterId()
{
    return characterId;
}

void Tab_Settings::restoreSettings()
{
    int language_index = loadSettings("language").toInt();
    //Load language
    on_cb_language_currentIndexChanged(language_index);

    //Set language in combo box
    ui->cb_language->blockSignals(true);
    ui->cb_language->setCurrentIndex(language_index);
    ui->cb_language->blockSignals(false);

    ui->cb_cheater->setChecked(loadSettings("Cheat mode").toInt());
}

Tab_Settings::~Tab_Settings()
{
    delete ui;
    delete validator;
}

void Tab_Settings::on_cb_saves_currentIndexChanged(int index)
{
    const auto &characters = JsonParser::characters();
    characterId = index >= 0 && index < characters.size() ? index : -1;
    const QSignalBlocker blocker(ui->le_maxIconUses);
    ui->le_maxIconUses->setEnabled(characterId >= 0 && JsonParser::canEdit());
    ui->le_maxIconUses->setText(characterId >= 0
        ? QString::number(characters[characterId].character["maxIconUses"].toInt())
        : QString());
    emit characterChanged();
}

void Tab_Settings::showSaveStatus(const QString &error)
{
    QString status = error;
    if (status.isEmpty() && JsonParser::characters().isEmpty())
        status = tr("No characters found. Editing is disabled.");
    saveStatus = status;
    ui->l_saveStatus->setText(status);
    ui->l_saveStatus->setVisible(!status.isEmpty());
}

void Tab_Settings::on_btn_restore_clicked()
{
    // DIG-120 will replace this temporary entry point with the full catalog
    // and restore UI. Storage and capture already work without a parsed save.
    QSettings settings("DeathWhisper", "TLHoN_SE");
    BackupService backups(JsonParser::paths(), BackupService::configuredStorageDirectory(settings));
    QMessageBox dialog(QMessageBox::Information, tr("Backup / recovery"),
        tr("Close the game before creating or recovering backups. Snapshots copy the complete save, "
           "not just the selected character. Restoring snapshots will be available in the Backup Manager.") +
        "\n\n" + tr("Save folder: %1").arg(JsonParser::paths().directory()) +
        "\n" + tr("Backup storage: %1").arg(backups.storageDirectory()),
        QMessageBox::Close, this);
    auto *create = dialog.addButton(tr("Create backup"), QMessageBox::ActionRole);
    auto *choose = dialog.addButton(tr("Choose backup folder"), QMessageBox::ActionRole);
    auto *legacy = dialog.addButton(tr("Import legacy backup"), QMessageBox::ActionRole);
    legacy->setEnabled(QFileInfo::exists(JsonParser::paths().legacyEditorBackup()));
    auto *open = dialog.addButton(tr("Open save folder"), QMessageBox::ActionRole);
    auto *reload = dialog.addButton(tr("Reload save"), QMessageBox::ActionRole);
    dialog.exec();
    if (dialog.clickedButton() == choose) {
        const QString directory = QFileDialog::getExistingDirectory(this, tr("Choose backup storage"),
            backups.storageDirectory());
        if (!directory.isEmpty()) {
            QString error;
            if (!BackupService::configureStorageDirectory(settings, JsonParser::paths(), directory, error))
                QMessageBox::warning(this, tr("Backup storage"), error);
        }
    } else if (dialog.clickedButton() == create || dialog.clickedButton() == legacy) {
        BackupSnapshot snapshot;
        QString error;
        const bool success = dialog.clickedButton() == create
            ? backups.createSnapshot({}, snapshot, error)
            : backups.importLegacy(tr("Legacy editor backup"), snapshot, error);
        if (success)
            QMessageBox::information(this, tr("Backup created"),
                tr("Snapshot verified and saved in %1").arg(backups.storageDirectory() + "/" + snapshot.id));
        else
            QMessageBox::warning(this, tr("Backup not created"), error);
    } else if (dialog.clickedButton() == open) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(JsonParser::paths().directory())))
            QMessageBox::warning(this, tr("Recovery"), tr("Could not open the save directory."));
    } else if (dialog.clickedButton() == reload) {
        QString error;
        const bool loaded = JsonParser::init(error);
        loadGameSaves();
        showSaveStatus(error);
        if (loaded)
            emit saveRestored();
    }
}

void Tab_Settings::on_cb_cheater_toggled(bool checked)
{
    qInfo() << "Cheat mode: " << checked;
    saveSettings("Cheat mode", QString::number(checked));
    emit modeToggled(checked);
}

void Tab_Settings::on_le_maxIconUses_editingFinished()
{
    if (characterId < 0) return;
    const int max = qMax(1, ui->le_maxIconUses->text().toInt());
    QString error;
    const bool success = JsonParser::mutateCharacter(characterId, [max](SaveCharacter &slot) {
        slot.character["maxIconUses"] = max;
    }, error);
    on_cb_saves_currentIndexChanged(characterId);
    if (!success)
        QMessageBox::warning(this, tr("Save not changed"), error);
}

void Tab_Settings::on_cb_language_currentIndexChanged(int index)
{
    qApp->removeTranslator(&translator);
    QString file;

    switch (index)
    {
    case 1:
        language = "ru";
        break;
    default:
        file = ":/translations/TLHoN_SE_en.qm";
        language = "en";
        break;
    }

    saveSettings("Language", QString::number(index));

    if(translator.load(file) && !file.isEmpty())
        qApp->installTranslator(&translator);

    qInfo() << "Changing language to [" << language <<']';
    emit ui->retranslateUi(this);
    ui->l_saveStatus->setText(saveStatus);
    emit languageChanged();

    on_cb_saves_currentIndexChanged(characterId);
}

void Tab_Settings::saveSettings(const QString &key, const QString &value)
{
    QSettings settings("DeathWhisper", "TLHoN_SE");
    settings.setValue(key, value);
}

QString Tab_Settings::loadSettings(const QString &key)
{
    QSettings settings("DeathWhisper", "TLHoN_SE");
    return settings.value(key).toString();
}
