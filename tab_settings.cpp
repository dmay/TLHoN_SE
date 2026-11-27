#include "tab_settings.h"
#include "ui_tab_settings.h"

#include "jsonparser.h"
#include "backupservice.h"
#include "restoreservice.h"
#include "stylefromfile.h"
#include "positiveintvalidator.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QInputDialog>
#include <QStringList>
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
    // DIG-120 will replace this entry point with the full catalog UI.
    QSettings settings("DeathWhisper", "TLHoN_SE");
    BackupService backups(JsonParser::paths(), BackupService::configuredStorageDirectory(settings));
    QMessageBox dialog(QMessageBox::Information, tr("Backup / recovery"),
        tr("Close the game before creating or recovering backups. Snapshots copy the complete save, "
           "not just the selected character. Finish Steam Cloud synchronization before restoring.") +
        "\n\n" + tr("Save folder: %1").arg(JsonParser::paths().directory()) +
        "\n" + tr("Backup storage: %1").arg(backups.storageDirectory()),
        QMessageBox::Close, this);
    const bool pending = RestoreService::hasPending(JsonParser::paths());
    auto *restore = dialog.addButton(tr("Restore snapshot"), QMessageBox::ActionRole);
    restore->setEnabled(!pending);
    auto *create = dialog.addButton(tr("Create backup"), QMessageBox::ActionRole);
    create->setEnabled(!pending);
    auto *choose = dialog.addButton(tr("Choose backup folder"), QMessageBox::ActionRole);
    auto *legacy = dialog.addButton(tr("Import legacy backup"), QMessageBox::ActionRole);
    legacy->setEnabled(!pending && QFileInfo::exists(JsonParser::paths().legacyEditorBackup()));
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
    } else if (dialog.clickedButton() == restore) {
        QList<BackupSnapshot> snapshots;
        QString error;
        if (!backups.list(snapshots, error)) {
            QMessageBox::warning(this, tr("Cannot list snapshots"), error);
            return;
        }
        QStringList options;
        for (const auto &snapshot : snapshots) {
            const auto kind = snapshot.kind == BackupKind::Legacy ? tr("Legacy") :
                snapshot.kind == BackupKind::Safety ? tr("Automatic safety") : tr("Full save");
            options.append(QString("%1 | %2 | %3 | %4").arg(
                snapshot.id, kind, snapshot.label, snapshot.valid ? tr("Verified") : tr("Invalid")));
        }
        if (options.isEmpty()) {
            QMessageBox::information(this, tr("Restore snapshot"), tr("No snapshots found. Import a legacy backup or choose the correct storage folder."));
            return;
        }
        bool accepted = false;
        const auto selected = QInputDialog::getItem(this, tr("Restore snapshot"), tr("Select snapshot"),
                                                    options, 0, false, &accepted);
        const auto index = options.indexOf(selected);
        if (accepted && index >= 0) restoreSnapshot(snapshots[index].id);
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
        JsonParser::init(error);
        loadGameSaves();
        showSaveStatus(error);
        emit saveRestored();
    }
}

void Tab_Settings::restoreSnapshot(const QString &id)
{
    QSettings settings("DeathWhisper", "TLHoN_SE");
    RestoreService service(JsonParser::paths(), BackupService::configuredStorageDirectory(settings));
    RestorePreview preview;
    QString error;
    if (!RestoreService::gameClosed(error) || !service.preview(id, preview, error)) {
        QMessageBox::warning(this, tr("Restore blocked"), error);
        return;
    }
    QString details = tr("Snapshot: %1\nSource: %2\nTarget: %3\n\nFiles:")
        .arg(preview.snapshot.id, preview.snapshot.sourceDirectory, preview.targetDirectory);
    QStringList summary;
    for (const auto &file : preview.snapshot.files) {
        if (file.state == BackupFileState::Present) {
            summary.append(tr("%1 — %2 bytes").arg(file.name, QString::number(file.size)));
            details += QString("\n%1 — %2 bytes; SHA-256 %3").arg(
                file.name, QString::number(file.size), QString::fromLatin1(file.sha256));
        } else {
            const auto state = file.state == BackupFileState::Missing
                ? tr("Missing (remove from target)") : tr("Not captured (leave unchanged)");
            summary.append(file.name + " — " + state);
            details += "\n" + file.name + " — " + state;
        }
    }
    const QString scope = preview.snapshot.kind == BackupKind::Legacy
        ? tr("Legacy primary-only restore: SaveData.txt will be replaced for ALL characters. "
             "The existing game companion SaveData.txt.bak will be left unchanged.")
        : tr("Full-save restore: ALL characters and the game's companion file state will be replaced.");
    QString warning = scope + "\n\n" +
        tr("Keep the game closed and finish Steam Cloud synchronization. The editor does not change cloud settings. "
           "A cloud conflict may overwrite the restored save; choose the local copy only after checking that it is the intended version. "
           "An automatic verified safety snapshot will preserve the current files before restore.");
    if (preview.primaryNeedsRecovery)
        warning += "\n\n" + tr("Recovery mode: the current primary is missing or corrupt. The safety snapshot will retain whatever bytes exist, including the exact absence of files.");
    QMessageBox confirm(QMessageBox::Warning, tr("Confirm restore"), warning,
                        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setDefaultButton(QMessageBox::No);
    confirm.setInformativeText(tr("Snapshot: %1\nTarget: %2\n%3").arg(
        preview.snapshot.id, preview.targetDirectory, summary.join("\n")));
    confirm.setDetailedText(details);
    if (confirm.exec() != QMessageBox::Yes) return;

    BackupSnapshot safety;
    const bool restored = service.restore(preview, safety, error);
    QString reloadError;
    if (RestoreService::hasPending(JsonParser::paths())) {
        JsonParser::invalidate();
        reloadError = error;
    } else {
        JsonParser::init(reloadError);
    }
    loadGameSaves();
    showSaveStatus(reloadError);
    emit saveRestored();
    if (restored) {
        QMessageBox::information(this, tr("Restore complete"),
            tr("Restored and verified. Pre-restore safety snapshot: %1").arg(safety.id) +
            (reloadError.isEmpty() ? QString() : "\n" + reloadError));
    } else {
        QMessageBox::warning(this, tr("Restore not completed"), error);
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
