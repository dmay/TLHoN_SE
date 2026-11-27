#include "backupmanager.h"
#include "jsonparser.h"
#include "restoreservice.h"
#include "savedocument.h"
#include "tab_settings.h"

#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace {
class DiskBackend final : public BackupManagerBackend
{
public:
    explicit DiskBackend(Tab_Settings *tab) : settings("DeathWhisper", "TLHoN_SE"), tab(tab) {}
    QString storageDirectory() const override { return BackupService::configuredStorageDirectory(settings); }
    QString saveDirectory() const override { return JsonParser::paths().directory(); }
    bool list(QList<BackupCatalogEntry> &entries, QString &error) override
    {
        const auto service = backups();
        QList<BackupSnapshot> snapshots;
        if (!service.list(snapshots, error)) return false;
        for (const auto &snapshot : snapshots) {
            BackupCatalogEntry entry{snapshot, false, snapshot.error};
            if (snapshot.valid) {
                QMap<QString, QByteArray> bytes;
                BackupSnapshot verified;
                if (service.readVerified(snapshot.id, verified, bytes, entry.diagnostic)) {
                    entry.snapshot = verified;
                    const auto name = verified.kind == BackupKind::Legacy
                        ? QString("SaveData.txt_bckp") : QString("SaveData.txt");
                    SaveDocument parsed;
                    entry.restorable = bytes.contains(name) && SaveDocument::parse(bytes.value(name), parsed, entry.diagnostic);
                    if (!bytes.contains(name))
                        entry.diagnostic = BackupManager::tr("В копии нет основного файла сохранения.");
                } else {
                    entry.snapshot.valid = false;
                }
            }
            entries.append(entry);
        }
        return true;
    }
    bool activeValid(QString &error) override
    {
        SaveStore store(JsonParser::paths());
        const bool valid = store.load(error);
        return valid;
    }
    bool gameClosed(QString &error) override { return RestoreService::gameClosed(error); }
    bool pendingRecovery() const override { return RestoreService::hasPending(JsonParser::paths()); }
    bool legacyAvailable() const override { return QFileInfo::exists(JsonParser::paths().legacyEditorBackup()); }
    bool create(const QString &label, bool legacy, BackupSnapshot &snapshot, QString &error) override
    {
        if (!gameClosed(error) || (!legacy && !activeValid(error))) return false;
        return legacy ? backups().importLegacy(label, snapshot, error)
                      : backups().createSnapshot(label, snapshot, error);
    }
    bool rename(const QString &id, const QString &label, QString &error) override
    {
        return gameClosed(error) && activeValid(error) && backups().renameLabel(id, label, error);
    }
    bool remove(const QString &id, QString &error) override
    {
        return gameClosed(error) && activeValid(error) && backups().deleteSnapshot(id, true, error);
    }
    bool chooseStorage(const QString &directory, QString &error) override
    {
        return BackupService::configureStorageDirectory(settings, JsonParser::paths(), directory, error);
    }
    QString restore(const QString &id, QWidget *parent) override { return tab->restoreSnapshot(id, parent); }
    QString reload() override { return tab->reloadSave(); }

private:
    BackupService backups() const { return BackupService(JsonParser::paths(), storageDirectory()); }
    QSettings settings;
    Tab_Settings *tab;
};

class BusyCursor
{
public:
    BusyCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~BusyCursor() { QApplication::restoreOverrideCursor(); }
};
}

BackupManager::BackupManager(BackupManagerBackend &backend, QWidget *parent)
    : QDialog(parent), backend(backend)
{
    setObjectName("backupManager");
    setWindowTitle(tr("Менеджер резервных копий"));
    resize(1080, 620);
    // The editor's retro stylesheet is intended for its tabs, not this
    // information-dense dialog. Keep native readable controls and labels.
    setStyleSheet("QDialog { background: palette(window); } "
                  "QLabel { font: 12px 'Segoe UI'; color: palette(window-text); } "
                  "QPushButton { font: 12px 'Segoe UI'; color: palette(button-text); "
                  "background: palette(button); border: 1px solid palette(mid); padding: 6px; } "
                  "QPushButton:hover { border-color: palette(highlight); } "
                  "QPushButton:disabled { color: palette(mid); }");
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Закройте игру и дождитесь завершения синхронизации Steam Cloud. Копии содержат всё сохранение, а не только выбранного персонажа."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    folders = new QLabel(this);
    folders->setTextFormat(Qt::PlainText);
    folders->setWordWrap(true);
    folders->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(folders);

    auto *folderActions = new QHBoxLayout;
    auto add = [this](QHBoxLayout *row, const QString &text, const char *name, auto action) {
        auto *button = new QPushButton(text, this);
        button->setObjectName(name);
        button->setAutoDefault(false);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, action);
        return button;
    };
    add(folderActions, tr("Выбрать папку копий"), "chooseBackupFolder", [this] { chooseFolder(); });
    add(folderActions, tr("Открыть папку копий"), "openBackupFolder", [this] { openFolder(backend.storageDirectory()); });
    add(folderActions, tr("Открыть папку сохранений"), "openSaveFolder", [this] { openFolder(backend.saveDirectory()); });
    add(folderActions, tr("Перезагрузить сохранение"), "reloadSave", [this] {
        feedback(backend.reload());
        refresh();
    });
    folderActions->addStretch();
    add(folderActions, tr("Обновить список"), "refreshBackups", [this] { refresh(); });
    layout->addLayout(folderActions);

    table = new QTableWidget(0, 6, this);
    table->setObjectName("snapshotTable");
    table->setHorizontalHeaderLabels({tr("Создано (местное время)"), tr("Название"), tr("Тип"), tr("Файлы / проверка"),
                                      tr("Байты"), tr("Источник")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 155);
    table->setColumnWidth(1, 180);
    table->setColumnWidth(2, 140);
    table->setColumnWidth(3, 200);
    table->setColumnWidth(4, 100);
    layout->addWidget(table, 3);

    details = new QPlainTextEdit(this);
    details->setObjectName("snapshotDetails");
    details->setReadOnly(true);
    layout->addWidget(details, 2);
    auto *actions = new QHBoxLayout;
    createButton = add(actions, tr("Создать копию"), "createBackup", [this] { capture(false); });
    legacyButton = add(actions, tr("Импорт старой копии"), "importLegacy", [this] { capture(true); });
    renameButton = add(actions, tr("Изменить название"), "editLabel", [this] { renameSelected(); });
    deleteButton = add(actions, tr("Удалить выбранную"), "deleteSnapshot", [this] { deleteSelected(); });
    restoreButton = add(actions, tr("Восстановить выбранную"), "restoreSnapshot", [this] { restoreSelected(); });
    actions->addStretch();
    add(actions, tr("Закрыть"), "closeManager", &QDialog::accept);
    layout->addLayout(actions);
    availability = new QLabel(this);
    availability->setObjectName("backupAvailability");
    availability->setTextFormat(Qt::PlainText);
    availability->setWordWrap(true);
    layout->addWidget(availability);
    status = new QLabel(this);
    status->setObjectName("backupStatus");
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setWordWrap(true);
    layout->addWidget(status);
    connect(table, &QTableWidget::itemSelectionChanged, this, &BackupManager::showSelection);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &BackupManager::updateActions);
    timer->start(1000);
    refresh();
}

void BackupManager::open(Tab_Settings *settings)
{
    DiskBackend backend(settings);
    BackupManager dialog(backend, settings);
    dialog.exec();
}

const BackupCatalogEntry *BackupManager::selected() const
{
    const int row = table->currentRow();
    return row >= 0 && row < entries.size() && !table->selectedItems().isEmpty() ? &entries[row] : nullptr;
}

void BackupManager::refresh(const QString &preferred)
{
    QString selectedId = preferred;
    if (selectedId.isEmpty() && selected()) selectedId = selected()->snapshot.id;
    BusyCursor busy;
    folders->setText(tr("Папка сохранений: %1\nПапка копий: %2").arg(backend.saveDirectory(), backend.storageDirectory()));
    const QSignalBlocker blocker(table);
    entries.clear();
    QString error;
    catalogReady = backend.list(entries, error);
    if (!catalogReady) {
        entries.clear();
        feedback(tr("Не удалось получить список копий: %1").arg(error));
    }
    activeReady = backend.activeValid(activeError);
    table->setRowCount(0);
    table->setRowCount(entries.size());
    int choose = -1;
    for (int row = 0; row < entries.size(); ++row) {
        const auto &entry = entries[row];
        const auto &s = entry.snapshot;
        qint64 bytes = 0;
        int present = 0;
        for (const auto &file : s.files) {
            if (file.state == BackupFileState::Present) { ++present; bytes += file.size; }
        }
        const QString kind = !s.valid && s.files.isEmpty() ? tr("Неизвестно") :
            s.kind == BackupKind::Safety ? tr("Автоматическая страховочная") :
            s.kind == BackupKind::Legacy ? tr("Старая (только основной файл)") : tr("Полное сохранение");
        const QString validity = !s.valid ? tr("Нарушена целостность") :
            !entry.restorable ? tr("Нельзя восстановить") : tr("Проверена / читается");
        const QStringList columns{
            s.createdAt.isValid() ? QLocale().toString(s.createdAt.toLocalTime(), QLocale::ShortFormat) : tr("Неизвестно"),
            s.label.isEmpty() ? tr("(Без названия)") : s.label, kind,
            tr("%1 / %2 — %3").arg(present).arg(s.files.size()).arg(validity),
            QLocale().toString(bytes), s.sourceDirectory};
        for (int col = 0; col < columns.size(); ++col) {
            auto *item = new QTableWidgetItem(columns[col]);
            item->setToolTip(columns[col]);
            table->setItem(row, col, item);
        }
        if (s.id == selectedId) choose = row;
    }
    if (choose < 0 && !entries.isEmpty()) choose = 0;
    if (choose >= 0) table->selectRow(choose);
    showSelection();
}

void BackupManager::showSelection()
{
    const auto *entry = selected();
    if (!entry) {
        details->setPlainText(tr("Выберите копию для просмотра файлов. Возможно, в этой папке пока нет копий."));
    } else {
        const auto &s = entry->snapshot;
        QString description = tr("Копия: %1\nИсточник: %2\nПрофиль: %3").arg(s.id, s.sourceDirectory, s.sourceProfile);
        for (const auto &file : s.files) {
            const QString state = file.state == BackupFileState::Present
                ? tr("Есть — %1 байт; SHA-256 %2").arg(file.size).arg(QString::fromLatin1(file.sha256))
                : file.state == BackupFileState::Missing ? tr("Отсутствует") : tr("Не скопирован");
            description += "\n" + file.name + " — " + state;
        }
        if (!entry->diagnostic.isEmpty()) description += "\n" + entry->diagnostic;
        if (s.kind == BackupKind::Safety)
            description += "\n" + tr("Автоматические страховочные копии нельзя переименовать или удалить.");
        details->setPlainText(description);
    }
    updateActions();
}

void BackupManager::updateActions()
{
    QString gameError;
    const bool closed = backend.gameClosed(gameError);
    const bool pending = backend.pendingRecovery();
    const bool ready = catalogReady && closed && !pending;
    const auto *entry = selected();
    const bool editable = entry && entry->snapshot.valid && entry->snapshot.kind != BackupKind::Safety;
    createButton->setEnabled(ready && activeReady);
    legacyButton->setEnabled(ready && backend.legacyAvailable());
    restoreButton->setEnabled(ready && entry && entry->restorable && entry->snapshot.valid);
    renameButton->setEnabled(ready && activeReady && editable);
    deleteButton->setEnabled(ready && activeReady && editable);
    if (!closed) availability->setText(gameError);
    else if (pending) availability->setText(tr("Восстановление прервано: перезагрузите сохранение для попытки восстановления."));
    else if (!activeReady) availability->setText(tr("Сохранение отсутствует или повреждено. Восстановите или импортируйте старую копию. %1").arg(activeError));
    else if (!catalogReady) availability->setText(tr("Папка копий недоступна. Выберите другую папку или обновите список."));
    else availability->setText(tr("Игра закрыта. Выберите проверенную копию или создайте новую."));
}

bool BackupManager::allowMutation(bool requiresActive)
{
    QString error;
    if (!backend.gameClosed(error)) feedback(error);
    else if (backend.pendingRecovery()) feedback(tr("Восстановление прервано: сначала перезагрузите сохранение."));
    else if (requiresActive && !backend.activeValid(error)) feedback(tr("Сохранение повреждено: %1").arg(error));
    else return true;
    refresh();
    return false;
}

void BackupManager::feedback(const QString &message)
{
    status->setText(message);
    status->repaint();
}

void BackupManager::capture(bool legacy)
{
    if (!allowMutation(!legacy)) return;
    bool ok = false;
    const auto label = QInputDialog::getText(this, tr("Создать копию"), tr("Название (необязательно)"), QLineEdit::Normal,
        legacy ? tr("Старая копия редактора") : QString(), &ok);
    if (!ok) { feedback(tr("Копирование отменено.")); return; }
    if (!allowMutation(!legacy)) return;
    feedback(tr("Создание и проверка копии…"));
    BackupSnapshot snapshot;
    QString error;
    bool success;
    {
        BusyCursor busy;
        success = backend.create(label, legacy, snapshot, error);
    }
    feedback(success ? tr("Копия создана и проверена: %1").arg(QDir(backend.storageDirectory()).filePath(snapshot.id))
                     : tr("Копия не создана: %1").arg(error));
    refresh(success ? snapshot.id : QString());
}

void BackupManager::renameSelected()
{
    if (!selected() || !allowMutation(true)) return;
    const auto snapshot = selected()->snapshot;
    bool ok = false;
    const auto label = QInputDialog::getText(this, tr("Изменить название"), tr("Название (необязательно)"), QLineEdit::Normal, snapshot.label, &ok);
    if (!ok) { feedback(tr("Изменение названия отменено.")); return; }
    if (!allowMutation(true)) return;
    QString error;
    const bool success = backend.rename(snapshot.id, label, error);
    feedback(success ? tr("Название изменено: %1").arg(snapshot.id) : tr("Название не изменено: %1").arg(error));
    refresh(snapshot.id);
}

void BackupManager::deleteSelected()
{
    if (!selected() || !allowMutation(true)) return;
    const auto snapshot = selected()->snapshot;
    QMessageBox confirm(QMessageBox::Warning, tr("Удалить копию"),
        tr("Удалить только эту копию безвозвратно? Сохранение и другие копии не будут изменены."),
        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setInformativeText(snapshot.label + "\n" + snapshot.id);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::No);
    if (confirm.exec() != QMessageBox::Yes) { feedback(tr("Удаление отменено.")); return; }
    if (!allowMutation(true)) return;
    QString error;
    const bool success = backend.remove(snapshot.id, error);
    feedback(success ? tr("Копия удалена: %1").arg(snapshot.id) : tr("Удаление не завершено: %1").arg(error));
    refresh();
}

void BackupManager::restoreSelected()
{
    if (!selected() || !selected()->restorable || !allowMutation(false)) return;
    const QString id = selected()->snapshot.id;
    feedback(tr("Подготовка восстановления…"));
    feedback(backend.restore(id, this));
    refresh(id);
}

void BackupManager::chooseFolder()
{
    const auto directory = QFileDialog::getExistingDirectory(this, tr("Выбрать хранилище копий"), backend.storageDirectory());
    if (directory.isEmpty()) return;
    QString error;
    if (!backend.chooseStorage(directory, error)) {
        feedback(tr("Папка копий не изменена: %1").arg(error));
        return;
    }
    feedback(tr("Выбрана другая папка копий. Существующие копии не перемещены."));
    refresh();
}

void BackupManager::openFolder(const QString &path)
{
    if (!QFileInfo(path).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        feedback(tr("Не удалось открыть папку: %1").arg(path));
}
