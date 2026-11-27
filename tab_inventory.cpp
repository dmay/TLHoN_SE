#include "tab_inventory.h"
#include "ui_tab_inventory.h"
#include "tab_settings.h"
#include "jsonparser.h"
#include "restoreservice.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QShowEvent>
#include <algorithm>

using ItemCatalog::Category;
using ItemCatalog::Entry;

namespace {
Inventory::ArrayKind destination(const Entry &entry)
{
    return entry.category == Category::Armor ? Inventory::ArrayKind::Generic : Inventory::ArrayKind::Weapon;
}

QString categoryText(Category category)
{
    switch (category) {
    case Category::Armor: return Tab_Inventory::tr("Броня");
    case Category::Weapon: return Tab_Inventory::tr("Оружие");
    case Category::Shield: return Tab_Inventory::tr("Щиты");
    default: return Tab_Inventory::tr("Другое");
    }
}

QString availabilityText(ItemCatalog::Availability value)
{
    switch (value) {
    case ItemCatalog::Availability::Validated: return Tab_Inventory::tr("Проверен");
    case ItemCatalog::Availability::Excluded: return Tab_Inventory::tr("Исключён");
    default: return Tab_Inventory::tr("Не проверен");
    }
}

QString dlcText(ItemCatalog::Dlc value)
{
    switch (value) {
    case ItemCatalog::Dlc::Base: return Tab_Inventory::tr("Основная игра");
    case ItemCatalog::Dlc::RequiresDlc: return Tab_Inventory::tr("Требует DLC");
    case ItemCatalog::Dlc::DlcPath: return Tab_Inventory::tr("Путь DLC");
    default: return Tab_Inventory::tr("Неизвестно");
    }
}
}

Tab_Inventory::Tab_Inventory(QWidget *parent) : QWidget(parent), ui(new Ui::Tab_Inventory)
{
    ui->setupUi(this);
    // The main window uses a dark background. Use ordinary, readable controls
    // within the existing pane instead of the old image placeholder.
    setStyleSheet("QLabel, QCheckBox { color: white; font: 12px sans-serif; }");
    auto *header = ui->catalogTable->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < header->count(); ++i)
        header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    ui->catalogTable->verticalHeader()->hide();
    connect(ui->discoverCatalog, &QPushButton::clicked, this, &Tab_Inventory::discoverCatalog);
    connect(ui->chooseCatalog, &QPushButton::clicked, this, &Tab_Inventory::chooseCatalog);
    connect(ui->chooseReview, &QPushButton::clicked, this, &Tab_Inventory::chooseReview);
    connect(ui->reloadCatalog, &QPushButton::clicked, this, [this] { loadCatalog(source, reviewPath); });
    connect(ui->catalogSearch, &QLineEdit::textChanged, this, &Tab_Inventory::renderRows);
    connect(ui->categoryFilter, &QComboBox::currentIndexChanged, this, &Tab_Inventory::renderRows);
    connect(ui->missingOnly, &QCheckBox::toggled, this, &Tab_Inventory::renderRows);
    connect(ui->dlcEntitled, &QCheckBox::toggled, this, [this] { ++revision; renderRows(); });
    connect(ui->bulkQualified, &QCheckBox::toggled, this, [this] { ++revision; });
    connect(ui->catalogTable, &QTableWidget::itemSelectionChanged, this, &Tab_Inventory::updateSelection);
    connect(ui->addSelected, &QPushButton::clicked, this, &Tab_Inventory::addSelected);
    retranslate();
}

Tab_Inventory::~Tab_Inventory() { delete ui; }

void Tab_Inventory::initSettings(Tab_Settings *settings)
{
    connect(settings, &Tab_Settings::characterChanged, this, [this] { ++revision; refresh(); });
    connect(settings, &Tab_Settings::saveRestored, this, [this] { ++revision; refresh(); });
    connect(settings, &Tab_Settings::editingAvailabilityChanged, this, &Tab_Inventory::refresh);
    connect(settings, &Tab_Settings::languageChanged, this, &Tab_Inventory::retranslate);
    refresh();
}

void Tab_Inventory::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void Tab_Inventory::retranslate()
{
    ui->retranslateUi(this);
    const int filter = qMax(0, ui->categoryFilter->currentIndex());
    const QSignalBlocker blocker(ui->categoryFilter);
    ui->categoryFilter->clear();
    ui->categoryFilter->addItem(tr("Все категории"));
    for (const auto category : {Category::Armor, Category::Weapon, Category::Shield})
        ui->categoryFilter->addItem(categoryText(category), int(category));
    ui->categoryFilter->setCurrentIndex(filter);
    refresh();
}

bool Tab_Inventory::loadCatalog(const ItemCatalog::Source &newSource, const QString &newReviewPath)
{
    ++revision;
    source = newSource;
    reviewPath = newReviewPath;
    catalogLoaded = ItemCatalog::load(source, catalog, catalogError, reviewPath);
    // A new catalog/review requires fresh entitlement and factory evidence.
    const QSignalBlocker dlcBlock(ui->dlcEntitled), bulkBlock(ui->bulkQualified);
    ui->dlcEntitled->setChecked(false);
    ui->bulkQualified->setChecked(false);
    refresh();
    return catalogLoaded;
}

void Tab_Inventory::chooseCatalog()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Выбрать catalog.json"),
        source.catalogPath, tr("JSON (*.json)"));
    if (!path.isEmpty()) loadCatalog({path, {}}, {});
}

void Tab_Inventory::discoverCatalog()
{
    ItemCatalog::Source found;
    QString error;
    if (ItemCatalog::locate(found, error)) {
        loadCatalog(found);
        return;
    }
    loadCatalog({});
    catalogError = error;
    refresh();
}

void Tab_Inventory::chooseReview()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Выбрать проверенный список"),
        reviewPath, tr("JSON (*.json)"));
    if (!path.isEmpty()) loadCatalog(source, path);
}

QString Tab_Inventory::characterLabel() const
{
    if (slotIndex < 0 || slotIndex >= JsonParser::characters().size()) return tr("Нет персонажа");
    return tr("Персонаж %1: %2").arg(slotIndex + 1).arg(JsonParser::characters()[slotIndex].character.value("name").toString());
}

QString Tab_Inventory::unavailableReason() const
{
    if (slotIndex < 0 || slotIndex >= JsonParser::characters().size()) return tr("Выберите персонажа в настройках.");
    if (!catalogLoaded) return tr("Откройте каталог игры на вкладке «Сумка».");
    if (reviewPath.isEmpty()) return tr("Выберите проверенный список для этой версии каталога. Непроверенные предметы нельзя добавлять.");
    if (!JsonParser::canEdit()) {
        QString gameError;
#ifdef Q_OS_WIN
        if (!RestoreService::gameClosed(gameError)) return gameError;
#endif
        return tr("Редактирование недоступно. Закройте игру, завершите синхронизацию Steam и перезагрузите или восстановите сохранение в настройках.");
    }
    return inventoryError;
}

void Tab_Inventory::refresh()
{
    slotIndex = Tab_Settings::getCharacterId();
    instances.clear();
    inventoryError.clear();
    if (slotIndex >= 0 && slotIndex < JsonParser::characters().size())
        Inventory::read(JsonParser::characters()[slotIndex], instances, inventoryError);
    ui->characterLabel->setText(characterLabel());
    ui->chooseReview->setEnabled(!source.catalogPath.isEmpty());
    ui->reloadCatalog->setEnabled(!source.catalogPath.isEmpty());
    const bool hasReview = catalogLoaded && !reviewPath.isEmpty();
    ui->dlcEntitled->setEnabled(hasReview);
    ui->bulkQualified->setEnabled(hasReview);
    if (!catalogLoaded) {
        ui->catalogStatus->setText(tr("Каталог не загружен. Найдите игру в Steam или выберите catalog.json.") +
            (catalogError.isEmpty() ? QString() : "\n" + catalogError));
    } else {
        ui->catalogStatus->setText(tr("Каталог: %1 (%2 записей); %3").arg(QFileInfo(source.catalogPath).fileName())
            .arg(catalog.entries.size()).arg(hasReview ? tr("Список: %1").arg(QFileInfo(reviewPath).fileName())
                : tr("Нет проверенного списка — только просмотр")));
    }
    ui->catalogStatus->setToolTip(source.catalogPath + "\n" + reviewPath + "\nSHA-256: " + catalog.sha256);
    renderRows();
}

int Tab_Inventory::owned(const Entry &entry) const
{
    return int(std::count_if(instances.cbegin(), instances.cend(), [&](const Inventory::Instance &item) {
        return item.guid == entry.guid && item.kind == destination(entry);
    }));
}

QString Tab_Inventory::selectedGuid() const
{
    const auto rows = ui->catalogTable->selectionModel()->selectedRows();
    if (rows.size() != 1) return {};
    const auto *item = ui->catalogTable->item(rows.first().row(), 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void Tab_Inventory::renderRows()
{
    const auto selected = selectedGuid();
    const QSignalBlocker blocker(ui->catalogTable);
    ui->catalogTable->clearContents();
    ui->catalogTable->setRowCount(0);
    const QString query = ui->catalogSearch->text().trimmed();
    const bool entitlement = ui->dlcEntitled->isChecked();
    const bool hasInventory = slotIndex >= 0 && inventoryError.isEmpty();
    int selectedRow = -1;
    for (const auto &entry : catalog.entries) {
        if (entry.category == Category::Other) continue;
        if (ui->categoryFilter->currentIndex() > 0 && ui->categoryFilter->currentData().toInt() != int(entry.category)) continue;
        if (!entry.displayName.contains(query, Qt::CaseInsensitive) && !entry.guid.contains(query, Qt::CaseInsensitive)) continue;
        const int count = owned(entry);
        if (ui->missingOnly->isChecked() && (!hasInventory || !entry.bulkEligible(entitlement) || count != 0)) continue;
        const int row = ui->catalogTable->rowCount();
        ui->catalogTable->insertRow(row);
        const QStringList fields{entry.displayName.isEmpty() ? entry.guid : entry.displayName,
            categoryText(entry.category), availabilityText(entry.availability), dlcText(entry.dlc),
            hasInventory ? QString::number(count) : QStringLiteral("—")};
        for (int column = 0; column < fields.size(); ++column) {
            auto *cell = new QTableWidgetItem(fields[column]);
            cell->setToolTip(entry.guid);
            if (column == 0) cell->setData(Qt::UserRole, entry.guid);
            ui->catalogTable->setItem(row, column, cell);
        }
        if (entry.guid == selected) selectedRow = row;
    }
    if (selectedRow >= 0) ui->catalogTable->selectRow(selectedRow);
    else ui->catalogTable->clearSelection();
    updateSelection();
}

void Tab_Inventory::updateSelection()
{
    const auto *entry = catalog.find(selectedGuid());
    const QString error = unavailableReason();
    ui->editStatus->setText(error);
    ui->editStatus->setVisible(!error.isEmpty());
    ui->addSelected->setEnabled(error.isEmpty() && entry && entry->bulkEligible(ui->dlcEntitled->isChecked()) && owned(*entry) == 0);
    if (!entry) {
        ui->itemPreview->setPlainText(tr("Выберите предмет. Точный GUID и массив назначения будут показаны здесь."));
        return;
    }
    QString text = tr("GUID: %1\nНазначение: %2 (%3)\nСтатус: %4; DLC: %5").arg(entry->guid,
        Inventory::arrayKey(destination(*entry)), categoryText(entry->category), availabilityText(entry->availability), dlcText(entry->dlc));
    if (!entry->slot.isEmpty()) text += "\n" + tr("Слот: %1").arg(entry->slot);
    if (slotIndex >= 0 && inventoryError.isEmpty()) text += "\n" + tr("Уже в сумке: %1").arg(owned(*entry));
    if (!entry->reason.isEmpty()) text += "\n" + entry->reason;
    if (entry->dlc == ItemCatalog::Dlc::RequiresDlc && !ui->dlcEntitled->isChecked())
        text += "\n" + tr("DLC не подтверждено.");
    ui->itemPreview->setPlainText(text);
}

void Tab_Inventory::reject(const QString &error)
{
    refresh();
    QMessageBox box(QMessageBox::Warning, tr("Сохранение не изменено"), error, QMessageBox::Ok, window());
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

void Tab_Inventory::addSelected()
{
    const auto guid = selectedGuid();
    if (guid.isEmpty()) { reject(tr("Выберите предмет.")); return; }
    grant({}, guid);
}

void Tab_Inventory::addAllArmor() { grant({Category::Armor}); }
void Tab_Inventory::addAllWeapons() { grant({Category::Weapon, Category::Shield}); }

void Tab_Inventory::grant(const QList<Category> &categories, const QString &guid)
{
    refresh();
    QString error = unavailableReason();
    if (!error.isEmpty()) { reject(error); return; }
    // Detect a replaced catalog/review even when the current preview would
    // be a no-op. Never silently switch to a different reviewed scope.
    ItemCatalog::Catalog verified;
    if (!ItemCatalog::load(source, verified, error, reviewPath) || verified.report() != catalog.report()) {
        loadCatalog(source, reviewPath);
        reject(tr("Каталог или проверенный список изменились. Обновите список и повторите добавление.") + "\n" + error);
        return;
    }
    const bool bulk = guid.isEmpty();
    if (bulk && !ui->bulkQualified->isChecked()) {
        reject(tr("Сначала проверьте создание предметов на тестовом сохранении и подтвердите проверку на вкладке «Сумка»."));
        return;
    }
    const bool entitlement = ui->dlcEntitled->isChecked();
    const int index = slotIndex;
    const auto previewRevision = revision;
    const SaveCharacter before = JsonParser::characters()[index];
    const auto expectedCatalog = catalog.report();
    QStringList missing;
    qsizetype eligible = 0, excluded = 0;
    QString scope;
    if (bulk) {
        scope = categories.contains(Category::Armor) ? tr("Вся проверенная броня (inventory)") : tr("Всё проверенное оружие и щиты (inventoryWeapons)");
        for (const auto &entry : catalog.entries) {
            if (!categories.contains(entry.category)) continue;
            if (!entry.bulkEligible(entitlement)) { ++excluded; continue; }
            ++eligible;
            if (owned(entry) == 0) missing.append(entry.guid);
        }
    } else {
        const auto *entry = catalog.find(guid);
        if (!entry || !entry->bulkEligible(entitlement)) { reject(tr("Предмет не разрешён в текущем каталоге.")); return; }
        scope = tr("%1 (%2)").arg(guid, Inventory::arrayKey(destination(*entry)));
        eligible = 1;
        if (owned(*entry) == 0) missing.append(guid);
    }
    if (eligible == 0) { reject(tr("Нет проверенных доступных предметов в этой категории.")); return; }
    if (missing.isEmpty()) {
        QMessageBox::information(window(), tr("Нечего добавлять"), tr("Новых предметов: 0. Все разрешённые предметы уже есть."));
        return;
    }
    QMessageBox confirm(QMessageBox::Question, tr("Подтвердить добавление"),
        tr("%1\n%2\nДобавить: %3; уже есть: %4; не разрешены: %5.\n\nСуществующие предметы и экипировка не будут изменены. Закройте игру, завершите синхронизацию Steam и создайте резервную копию. Значения новых предметов требуют проверки в игре.")
        .arg(characterLabel(), scope).arg(missing.size()).arg(eligible - missing.size()).arg(excluded),
        QMessageBox::Yes | QMessageBox::No, window());
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::No);
    confirm.setDetailedText(missing.join("\n"));
    if (confirm.exec() != QMessageBox::Yes) return;

    if (revision != previewRevision || Tab_Settings::getCharacterId() != index ||
            index >= JsonParser::characters().size()) {
        reject(tr("Персонаж или настройки каталога изменились. Повторите добавление."));
        return;
    }
    // The local review is trusted input, but it and the catalog can change
    // between rendering and confirmation. Reload both and verify the preview.
    if (!ItemCatalog::load(source, verified, error, reviewPath) || verified.report() != expectedCatalog) {
        loadCatalog(source, reviewPath);
        reject(tr("Каталог или проверенный список изменились. Обновите список и повторите добавление.") + "\n" + error);
        return;
    }
    qsizetype added = 0;
    const bool success = JsonParser::mutateCharacterChecked(index, [&](SaveCharacter &candidate, QString &mutationError) {
        if (candidate.character != before.character || candidate.info != before.info || candidate.quests != before.quests) {
            mutationError = tr("Сохранение изменилось после подтверждения. Повторите добавление.");
            return false;
        }
        if (!Inventory::addMissing(candidate, verified, missing, added, mutationError, entitlement)) return false;
        if (added != missing.size()) {
            mutationError = tr("Количество отсутствующих предметов изменилось. Повторите добавление.");
            return false;
        }
        return true;
    }, error);
    if (!success) { reject(error); return; }
    refresh();
    emit inventoryChanged();
    QMessageBox::information(window(), tr("Предметы добавлены"), tr("Добавлено предметов: %1.").arg(added));
}
