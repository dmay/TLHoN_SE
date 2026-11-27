#include "tab_inventory.h"
#include "ui_tab_inventory.h"
#include "tab_settings.h"
#include "jsonparser.h"
#include "restoreservice.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QHash>
#include <QBrush>
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
    setStyleSheet("QLabel, QCheckBox { color: white; font: 12px sans-serif; }"
                  "QWidget#ownedPage, QWidget#catalogPage { background: #282828; }");
    for (auto *table : {ui->ownedTable, ui->propertyTable}) {
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
    }
    ui->ownedTable->setColumnWidth(0, 185);
    ui->ownedTable->setColumnWidth(1, 115);
    ui->ownedTable->setColumnWidth(2, 155);
    ui->ownedTable->setColumnWidth(3, 100);
    ui->propertyTable->setColumnWidth(0, 140);
    ui->propertyTable->setColumnWidth(1, 145);
    ui->propertyTable->setColumnWidth(2, 125);
    ui->ownedSplitter->setStretchFactor(0, 1);
    ui->ownedSplitter->setStretchFactor(1, 1);
    connect(ui->ownedSearch, &QLineEdit::textChanged, this, &Tab_Inventory::renderOwnedRows);
    for (auto *filter : {ui->arrayFilter, ui->ownedCategoryFilter, ui->stateFilter})
        connect(filter, &QComboBox::currentIndexChanged, this, &Tab_Inventory::renderOwnedRows);
    connect(ui->ownedTable, &QTableWidget::itemSelectionChanged, this, &Tab_Inventory::selectOwned);
    connect(ui->propertyState, &QComboBox::currentIndexChanged, this, &Tab_Inventory::updatePropertyPreview);
    connect(ui->applyProperties, &QPushButton::clicked, this, &Tab_Inventory::applySelectedProperties);
    connect(ui->cancelProperties, &QPushButton::clicked, this, &Tab_Inventory::cancelSelectedProperties);
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
    connect(settings, &Tab_Settings::saveContextChanging, this, [this](bool *allowed) {
        if (*allowed && !confirmDiscardDraft()) *allowed = false;
    }, Qt::DirectConnection);
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

    const auto populate = [](QComboBox *box, const QStringList &labels) {
        const int previous = qMax(0, box->currentIndex());
        const QSignalBlocker keep(box);
        box->clear();
        box->addItems(labels);
        box->setCurrentIndex(previous);
    };
    populate(ui->arrayFilter, {tr("Все массивы"), "inventory", "inventoryWeapons"});
    populate(ui->ownedCategoryFilter, {tr("Все категории"), categoryText(Category::Armor),
        categoryText(Category::Weapon), categoryText(Category::Shield), categoryText(Category::Other)});
    populate(ui->stateFilter, {tr("Все состояния"), tr("Просмотрен"), tr("Не просмотрен"),
        tr("Пробуждён"), tr("Освобождён")});
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
    emit catalogChanged();
    return catalogLoaded;
}

bool Tab_Inventory::verifyEquipmentCatalog(ItemCatalog::Catalog &verified, QString &error) const
{
    if (!catalogLoaded) {
        verified = {};
        error.clear();
        return true; // fixture-observed eligibility only
    }
    if (!ItemCatalog::load(source, verified, error, reviewPath)) return false;
    if (verified.report() != catalog.report()) {
        error = tr("Каталог или проверенный список изменились. Перезагрузите каталог.");
        return false;
    }
    return true;
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
    refreshProperties();
    renderOwnedRows();
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

namespace {
QString jsonText(const QJsonValue &value)
{
    const auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QString instanceKey(Inventory::ArrayKind kind, const QString &id)
{
    return Inventory::arrayKey(kind) + ":" + id;
}
}

QString Tab_Inventory::displayName(const Inventory::Instance &item) const
{
    const auto *entry = catalogLoaded ? catalog.find(item.guid) : nullptr;
    return entry && !entry->displayName.isEmpty() ? entry->displayName : item.guid;
}

void Tab_Inventory::renderOwnedRows()
{
    const QSignalBlocker blocker(ui->ownedTable);
    ui->ownedTable->clearContents();
    ui->ownedTable->setRowCount(0);
    QHash<QString, int> copies;
    for (const auto &item : instances) ++copies[instanceKey(item.kind, item.guid)];
    const QString query = ui->ownedSearch->text().trimmed();
    const int arrayFilter = ui->arrayFilter->currentIndex();
    const int categoryFilter = ui->ownedCategoryFilter->currentIndex();
    const int stateFilter = ui->stateFilter->currentIndex();
    const QList<Category> categories{Category::Other, Category::Armor, Category::Weapon, Category::Shield, Category::Other};

    for (const auto &item : instances) {
        const auto *entry = catalogLoaded ? catalog.find(item.guid) : nullptr;
        const Category category = entry ? entry->category : Category::Other;
        if (arrayFilter > 0 && arrayFilter != (item.kind == Inventory::ArrayKind::Generic ? 1 : 2)) continue;
        if (categoryFilter > 0 && category != categories.at(categoryFilter)) continue;
        const bool seen = item.fields.value("hasSeen").toBool();
        const bool awake = item.fields.value("isAwake").toBool();
        const bool hoovered = item.fields.value("isHoovered").toBool();
        if ((stateFilter == 1 && !seen) || (stateFilter == 2 && seen) ||
            (stateFilter == 3 && !awake) || (stateFilter == 4 && !hoovered)) continue;
        const auto name = displayName(item);
        if (!name.contains(query, Qt::CaseInsensitive) && !item.guid.contains(query, Qt::CaseInsensitive) &&
            !item.instanceId.contains(query, Qt::CaseInsensitive)) continue;
        QStringList state{seen ? tr("Просмотрен") : tr("Не просмотрен")};
        if (awake) state.append(tr("Пробуждён"));
        if (hoovered) state.append(tr("Освобождён"));
        const QStringList values{name, Inventory::arrayKey(item.kind), item.instanceId,
            state.join(", "), QString::number(copies.value(instanceKey(item.kind, item.guid)))};
        const int row = ui->ownedTable->rowCount();
        ui->ownedTable->insertRow(row);
        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            cell->setToolTip(item.guid + "\n" + categoryText(category) + "\n" + item.instanceId + "\n" + state.join(", "));
            if (column == 0) {
                cell->setData(Qt::UserRole, item.instanceId);
                cell->setData(Qt::UserRole + 1, int(item.kind));
            }
            ui->ownedTable->setItem(row, column, cell);
        }
        if (propertySelection.slotIndex == slotIndex && propertySelection.item.kind == item.kind &&
            propertySelection.item.instanceId == item.instanceId)
            ui->ownedTable->selectRow(row);
    }
    ui->ownedStatus->setText(slotIndex < 0 ? tr("Выберите персонажа в настройках.") :
        !inventoryError.isEmpty() ? tr("Не удалось прочитать инвентарь: %1").arg(inventoryError) :
        instances.isEmpty() ? tr("Инвентарь пуст.") :
        tr("Показано: %1 из %2 экземпляров.").arg(ui->ownedTable->rowCount()).arg(instances.size()));
}

QVariantMap Tab_Inventory::originalState() const
{
    if (propertySelection.slotIndex < 0) return {};
    const auto fields = propertySelection.item.fields;
    QVariantMap state{{"hasSeen", fields.value("hasSeen").toBool()}};
    if (propertySelection.item.kind == Inventory::ArrayKind::Generic)
        state.insert("uses", fields.value("uses").toInt());
    else {
        state.insert("level", fields.value("level").toInt());
        state.insert("infusionType", fields.value("infusionType").toInt());
    }
    return state;
}

bool Tab_Inventory::hasDraft() const
{
    return propertySelection.slotIndex >= 0 &&
        ui->propertyState->currentIndex() >= 0 &&
        ui->propertyState->currentData().toMap() != originalState();
}

bool Tab_Inventory::confirmDiscardDraft()
{
    if (!hasDraft()) return true;
    QMessageBox box(QMessageBox::Question, tr("Несохранённые изменения"),
        tr("Отменить неподтверждённые изменения экземпляра? Сохранение не будет изменено."),
        QMessageBox::Yes | QMessageBox::No, window());
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return false;
    cancelSelectedProperties();
    return true;
}

void Tab_Inventory::selectOwned()
{
    const auto rows = ui->ownedTable->selectionModel()->selectedRows();
    if (rows.size() != 1) return;
    const auto *cell = ui->ownedTable->item(rows.first().row(), 0);
    if (!cell) return;
    // Copy identity before any modal dialog allows the UI to refresh.
    const auto instanceId = cell->data(Qt::UserRole).toString();
    const auto kind = Inventory::ArrayKind(cell->data(Qt::UserRole + 1).toInt());
    const int requestedSlot = slotIndex;
    const auto requestedRevision = revision;
    if (propertySelection.slotIndex == slotIndex && propertySelection.item.kind == kind &&
        propertySelection.item.instanceId == instanceId) return;
    if (!confirmDiscardDraft()) { renderOwnedRows(); return; }
    QString error;
    propertySelection = {};
    if (requestedRevision != revision || requestedSlot != Tab_Settings::getCharacterId() ||
        !Inventory::selectProperties(JsonParser::characters(), requestedSlot, kind, instanceId, propertySelection, error)) {
        renderProperties();
        ui->propertyStatus->setText(tr("Невозможно выбрать экземпляр: %1").arg(error));
        return;
    }
    renderProperties();
}

void Tab_Inventory::refreshProperties()
{
    const auto pending = hasDraft() ? ui->propertyState->currentData().toMap() : QVariantMap{};
    if (propertySelection.slotIndex >= 0) {
        Inventory::PropertySelection current;
        QString error;
        // Never silently rebase a draft onto a different or changed record.
        if (propertySelection.slotIndex != slotIndex ||
            !Inventory::selectProperties(JsonParser::characters(), slotIndex, propertySelection.item.kind,
                propertySelection.item.instanceId, current, error) ||
            current.characterGuid != propertySelection.characterGuid ||
            current.item.fields != propertySelection.item.fields) {
            propertySelection = {};
            renderProperties();
            ui->propertyStatus->setText(tr("Экземпляр или персонаж изменился. Выберите предмет снова."));
            return;
        }
    }
    renderProperties(pending);
    if (!pending.isEmpty() && !hasDraft())
        ui->propertyStatus->setText(tr("Допустимые состояния изменились. Несохранённые изменения отменены."));
}

QString Tab_Inventory::propertyEditBlocker() const
{
    if (propertySelection.slotIndex < 0) return {};
    if (!JsonParser::canEdit()) return tr("Редактирование недоступно. Закройте игру, завершите синхронизацию Steam и перезагрузите или восстановите сохранение в настройках.");
    return propertyOptions.readOnlyReason;
}

void Tab_Inventory::renderProperties(const QVariantMap &pending)
{
    const QSignalBlocker blocker(ui->propertyState);
    ui->propertyState->clear();
    propertyOptions = {};
    if (propertySelection.slotIndex >= 0) {
        const auto &item = propertySelection.item;
        propertyOptions = Inventory::propertyOptions(JsonParser::characters().at(slotIndex), item);
        ui->selectedInstance->setText(tr("%1 — %2; ID: %3").arg(displayName(item), Inventory::arrayKey(item.kind), item.instanceId));
        const auto seenText = [](bool seen) { return seen ? Tab_Inventory::tr("да") : Tab_Inventory::tr("нет"); };
        // All selectable choices are typed, complete service-approved tuples.
        // This avoids offering an invalid Cartesian product of field ranges.
        for (const auto &state : propertyOptions.genericStates)
            ui->propertyState->addItem(tr("uses: %1; hasSeen: %2").arg(state.uses).arg(seenText(state.hasSeen)),
                QVariantMap{{"uses", state.uses}, {"hasSeen", state.hasSeen}});
        for (const auto &state : propertyOptions.weaponStates)
            ui->propertyState->addItem(tr("level: %1; infusionType: %2; hasSeen: %3").arg(state.level).arg(state.infusionType).arg(seenText(state.hasSeen)),
                QVariantMap{{"level", state.level}, {"infusionType", state.infusionType}, {"hasSeen", state.hasSeen}});
        const auto wanted = pending.isEmpty() ? originalState() : pending;
        int baseline = -1, target = -1;
        for (int i = 0; i < ui->propertyState->count(); ++i) {
            const auto map = ui->propertyState->itemData(i).toMap();
            if (map == originalState()) baseline = i;
            if (map == wanted) target = i;
        }
        ui->propertyState->setCurrentIndex(target >= 0 ? target : baseline);
    } else {
        ui->selectedInstance->setText(tr("Выберите экземпляр для просмотра свойств."));
    }
    updatePropertyPreview();
}

void Tab_Inventory::updatePropertyPreview()
{
    const QSignalBlocker blocker(ui->propertyTable);
    ui->propertyTable->setRowCount(0);
    ui->propertyTable->clearContents();
    const auto blocked = propertyEditBlocker();
    const bool selected = propertySelection.slotIndex >= 0;
    const bool alternatives = selected && ui->propertyState->count() > 1 && blocked.isEmpty();
    ui->propertyState->setEnabled(alternatives);
    ui->applyProperties->setEnabled(alternatives && hasDraft());
    ui->cancelProperties->setEnabled(hasDraft());
    if (!selected) {
        ui->propertyStatus->clear();
        return;
    }
    const auto fields = propertySelection.item.fields;
    const auto proposed = ui->propertyState->currentData().toMap();
    const auto fieldReason = [&](const QString &key) -> QString {
        if (key == "GUID" || key == "instanceIDOnCreation") return tr("Идентификатор (только чтение)");
        if (key == "isAwake" || key == "isHoovered") return propertyOptions.flagsReadOnlyReason;
        if (key == "newGamePlusCount") return propertyOptions.ngPlusReadOnlyReason;
        if (key == "uses") return propertyOptions.usesReadOnlyReason;
        if (key == "hasSeen") return propertyOptions.hasSeenReadOnlyReason;
        if (key == "level") return propertyOptions.levelReadOnlyReason;
        if (key == "infusionType") return propertyOptions.infusionReadOnlyReason;
        return tr("Неподдерживаемое поле (только чтение)");
    };
    // Always show the complete saved record, even unknown nested values.
    // Every detail cell is read-only; only the tuple selector can stage changes.
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const int row = ui->propertyTable->rowCount();
        ui->propertyTable->insertRow(row);
        const QString before = jsonText(it.value());
        const QString after = proposed.contains(it.key()) ?
            jsonText(QJsonValue::fromVariant(proposed.value(it.key()))) : before;
        const auto reason = fieldReason(it.key());
        const QStringList values{it.key(), before, after,
            reason.isEmpty() && blocked.isEmpty() ? tr("Допустимое состояние") :
            tr("Только чтение: %1").arg(reason.isEmpty() ? blocked : reason)};
        for (int col = 0; col < values.size(); ++col) {
            auto *cell = new QTableWidgetItem(values.at(col));
            cell->setToolTip(values.at(col));
            cell->setFlags(cell->flags().setFlag(Qt::ItemIsEditable, false));
            if (col == 2 && before != after) cell->setBackground(QBrush(QColor("#fff0b3")));
            ui->propertyTable->setItem(row, col, cell);
        }
    }
    if (!blocked.isEmpty()) ui->propertyStatus->setText(blocked);
    else if (hasDraft()) ui->propertyStatus->setText(tr("Изменения не сохранены. Примените или отмените."));
    else if (alternatives) ui->propertyStatus->setText(tr("Данные тестовых сохранений, не проверено в игре: %1").arg(propertyOptions.evidence));
    else ui->propertyStatus->setText(tr("Нет других допустимых состояний."));
}

void Tab_Inventory::cancelSelectedProperties()
{
    renderProperties();
}

void Tab_Inventory::applySelectedProperties()
{
    if (!hasDraft()) return;
    const auto state = ui->propertyState->currentData().toMap();
    Inventory::PropertyEdits edits;
    if (state.contains("uses")) edits.uses = state.value("uses").toInt();
    if (state.contains("level")) edits.level = state.value("level").toInt();
    if (state.contains("infusionType")) edits.infusionType = state.value("infusionType").toInt();
    if (state.contains("hasSeen")) edits.hasSeen = state.value("hasSeen").toBool();
    const auto selected = propertySelection;
    QString error;
    // The service checks the captured complete state, live selected slot,
    // exact record and on-disk bytes again before performing one commit.
    if (!Inventory::applyProperties(Tab_Settings::getCharacterId(), selected, edits, error)) {
        refresh();
        ui->propertyStatus->setText(tr("Сохранение не изменено: %1").arg(error));
        return;
    }
    if (!Inventory::selectProperties(JsonParser::characters(), Tab_Settings::getCharacterId(),
            selected.item.kind, selected.item.instanceId, propertySelection, error))
        propertySelection = {};
    // The old draft is now committed. Do not reapply it on the new selection.
    renderProperties();
    refresh();
    emit inventoryChanged();
    ui->propertyStatus->setText(tr("Изменения сохранены."));
}
