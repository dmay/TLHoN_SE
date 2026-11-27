#include "tab_settings.h"
#include "ui_tab_settings.h"

#include "jsonparser.h"
#include "backupservice.h"
#include "backupmanager.h"
#include "restoreservice.h"
#include "stylefromfile.h"
#include "positiveintvalidator.h"

#include <QMessageBox>
#include <QStringList>
#include <QSignalBlocker>
#include <QTimer>
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
    ui->l_saveStatus->setTextFormat(Qt::PlainText);
    loadGameSaves();
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Tab_Settings::refreshEditingAvailability);
    timer->start(1000);
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
    int language_index = loadSettings("Language").toInt();
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
    refreshEditingAvailability();
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
    refreshEditingAvailability();
}

void Tab_Settings::refreshEditingAvailability()
{
    const QSignalBlocker blocker(ui->le_maxIconUses);
    QString gameError;
#ifdef Q_OS_WIN
    RestoreService::gameClosed(gameError);
#endif
    const bool enabled = characterId >= 0 && JsonParser::canEdit() && gameError.isEmpty();
    ui->le_maxIconUses->setEnabled(enabled);
    QString status = saveStatus;
    if (!gameError.isEmpty())
        status += (status.isEmpty() ? QString() : "\n") + gameError;
    else if (!enabled && status.isEmpty())
        status = tr("Editing disabled. Reload or recover the save.");
    ui->l_saveStatus->setText(status);
    ui->l_saveStatus->setVisible(!status.isEmpty());
    if (enabled != editingAvailable) {
        editingAvailable = enabled;
        emit editingAvailabilityChanged();
    }
}

void Tab_Settings::on_btn_restore_clicked()
{
    BackupManager::open(this);
}

QString Tab_Settings::reloadSave()
{
    QString error;
    const bool loaded = JsonParser::init(error);
    loadGameSaves();
    showSaveStatus(error);
    emit saveRestored();
    return loaded && error.isEmpty() ? tr("Save reloaded.") : tr("Reload result: %1").arg(error);
}

QString Tab_Settings::restoreSnapshot(const QString &id, QWidget *parent)
{
    QWidget *owner = parent ? parent : this;
    QSettings settings("DeathWhisper", "TLHoN_SE");
    RestoreService service(JsonParser::paths(), BackupService::configuredStorageDirectory(settings));
    RestorePreview preview;
    QString error;
    if (!RestoreService::gameClosed(error) || !service.preview(id, preview, error)) {
        QMessageBox::warning(owner, tr("Restore blocked"), error);
        return tr("Restore blocked: %1").arg(error);
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
                        QMessageBox::Yes | QMessageBox::No, owner);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::No);
    confirm.setInformativeText(tr("Snapshot: %1\nTarget: %2\n%3").arg(
        preview.snapshot.id, preview.targetDirectory, summary.join("\n")));
    confirm.setDetailedText(details);
    if (confirm.exec() != QMessageBox::Yes) return tr("Restore canceled.");

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
        const QString result = tr("Restored and verified. Pre-restore safety snapshot: %1").arg(safety.id) +
            (reloadError.isEmpty() ? QString() : "\n" + tr("Reload warning: %1").arg(reloadError));
        if (reloadError.isEmpty())
            QMessageBox::information(owner, tr("Restore complete"), result);
        else
            QMessageBox::warning(owner, tr("Restore complete; reload needs attention"), result);
        return result;
    }
    const QString result = tr("Restore not completed: %1").arg(error) +
        (reloadError.isEmpty() ? QString() : "\n" + reloadError);
    QMessageBox::warning(owner, tr("Restore not completed"), result);
    return result;
}

void Tab_Settings::on_cb_cheater_toggled(bool checked)
{
    qInfo() << "Cheat mode: " << checked;
    saveSettings("Cheat mode", QString::number(checked));
    emit modeToggled(checked);
}

void Tab_Settings::on_le_maxIconUses_editingFinished()
{
    if (characterId < 0 || characterId >= JsonParser::characters().size() || !ui->le_maxIconUses->isEnabled()) return;
    const int value = ui->le_maxIconUses->text().toInt();
    if (value == JsonParser::characters()[characterId].character["maxIconUses"].toInt()) return;
    const int max = qMax(1, value);
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
    refreshEditingAvailability();
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
