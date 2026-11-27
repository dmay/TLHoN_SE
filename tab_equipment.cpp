#include "tab_equipment.h"
#include "ui_tab_equipment.h"
#include "stylefromfile.h"
#include "tab_settings.h"
#include "tab_inventory.h"
#include "jsonparser.h"

#include <QComboBox>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStandardItemModel>

using Equipment::Kind;

namespace {
bool dirty(const Equipment::Changes &changes)
{
    return !changes.selections.isEmpty() || changes.rightHandIndex ||
           changes.leftHandIndex || changes.consumableIndex;
}

// Capture only equipment and ownership fields. Ordinary Stats edits are not
// equipment changes and must not invalidate an otherwise current draft.
QJsonObject equipmentContext(const SaveCharacter &character)
{
    QJsonObject result;
    for (const auto &key : {"guid", "name", "inventory", "inventoryWeapons",
                           "rightHandEquipIndex", "leftHandEquipIndex", "selectedConsumableIndex"})
        result.insert(key, character.character.value(key));
    for (const auto kind : {Kind::Helm, Kind::Torso, Kind::Arms, Kind::Legs,
                            Kind::RightHand, Kind::LeftHand, Kind::Ring, Kind::Consumable}) {
        const auto key = Equipment::field(kind);
        result.insert(key, character.character.value(key));
    }
    return result;
}
}

Tab_Equipment::Tab_Equipment(QWidget *parent) :
    QWidget(parent), ui(new Ui::Tab_Equipment)
{
    ui->setupUi(this);
    setStyleFromFile(this, ":/Resources/StyleSheets/Tab_Equipment.qss");
    bindings = {
        {{Kind::Helm}, ui->slotHelm}, {{Kind::Torso}, ui->slotTorso},
        {{Kind::Arms}, ui->slotArms}, {{Kind::Legs}, ui->slotLegs},
        {{Kind::RightHand, 0}, ui->slotRight1}, {{Kind::RightHand, 1}, ui->slotRight2},
        {{Kind::LeftHand, 0}, ui->slotLeft1}, {{Kind::LeftHand, 1}, ui->slotLeft2},
        {{Kind::Ring, 0}, ui->slotRing1}, {{Kind::Ring, 1}, ui->slotRing2},
        {{Kind::Consumable, 0}, ui->slotConsumable1}, {{Kind::Consumable, 1}, ui->slotConsumable2},
        {{Kind::Consumable, 2}, ui->slotConsumable3}, {{Kind::Consumable, 3}, ui->slotConsumable4},
        {{Kind::Consumable, 4}, ui->slotConsumable5}
    };
    for (const auto &binding : bindings)
        connect(binding.box, &QComboBox::currentIndexChanged, this, &Tab_Equipment::updatePreview);
    for (auto *box : {ui->activeRight, ui->activeLeft, ui->activeConsumable})
        connect(box, &QComboBox::currentIndexChanged, this, &Tab_Equipment::updatePreview);
    connect(ui->applyEquipment, &QPushButton::clicked, this, &Tab_Equipment::apply);
    connect(ui->cancelEquipment, &QPushButton::clicked, this, [this] { populate(); });
    ui->characterLabel->setTextFormat(Qt::PlainText);
    ui->equipmentStatus->setTextFormat(Qt::PlainText);
}

Tab_Equipment::~Tab_Equipment() { delete ui; }

void Tab_Equipment::initSettings(Tab_Settings *settings, Tab_Inventory *items)
{
    inventory = items;
    connect(settings, &Tab_Settings::saveContextChanging, this, [this](bool *allowed) {
        if (*allowed && !confirmDiscardDraft()) *allowed = false;
    }, Qt::DirectConnection);
    connect(settings, &Tab_Settings::characterChanged, this, [this] {
        // Settings emits this after language and ordinary Stats/Settings updates
        // too, so retain a draft only if the equipment context is unchanged.
        const auto &characters = JsonParser::characters();
        if (valid && characterIndex == Tab_Settings::getCharacterId() &&
            characterIndex >= 0 && characterIndex < characters.size() &&
            baseline == equipmentContext(characters.at(characterIndex)))
            updatePreview();
        else
            refresh();
    });
    connect(settings, &Tab_Settings::saveRestored, this, &Tab_Equipment::refresh);
    connect(settings, &Tab_Settings::editingAvailabilityChanged, this, &Tab_Equipment::updatePreview);
    connect(settings, &Tab_Settings::languageChanged, this, &Tab_Equipment::retranslate);
    connect(items, &Tab_Inventory::inventoryChanged, this, &Tab_Equipment::refresh);
    connect(items, &Tab_Inventory::catalogChanged, this, &Tab_Equipment::refresh);
    populate();
}

QString Tab_Equipment::choiceText(const Equipment::Slot &slot) const
{
    if (slot.status == Equipment::Status::Empty)
        return tr("Снять (%1)").arg(slot.reference);
    const QString identity = slot.displayName == slot.reference ? slot.reference :
        QString("%1 [%2]").arg(slot.displayName, slot.reference);
    return slot.editable() ? identity : tr("Только чтение: %1").arg(identity);
}

QString Tab_Equipment::choiceDetails(const Equipment::Slot &slot) const
{
    QString text = choiceText(slot);
    if (!slot.guid.isEmpty()) text += "\nGUID: " + slot.guid;
    if (!slot.diagnostic.isEmpty()) text += "\n" + slot.diagnostic;
    return text;
}

Equipment::Changes Tab_Equipment::draft() const
{
    Equipment::Changes changes;
    if (!valid) return changes;
    for (const auto &binding : bindings) {
        const auto *stored = model.find(binding.position);
        if (stored && stored->editable() && binding.box->currentIndex() >= 0) {
            const auto reference = binding.box->currentData().toString();
            if (reference != stored->reference)
                changes.selections.append({binding.position, reference});
        }
    }
    const auto changedIndex = [](QComboBox *box, int original) -> std::optional<int> {
        if (box->currentIndex() >= 0 && box->currentData().toInt() != original)
            return box->currentData().toInt();
        return std::nullopt;
    };
    changes.rightHandIndex = changedIndex(ui->activeRight, model.rightHandIndex);
    changes.leftHandIndex = changedIndex(ui->activeLeft, model.leftHandIndex);
    changes.consumableIndex = changedIndex(ui->activeConsumable, model.consumableIndex);
    return changes;
}

bool Tab_Equipment::hasDraft() const { return dirty(draft()); }

bool Tab_Equipment::confirmDiscardDraft()
{
    if (!hasDraft()) return true;
    return QMessageBox::question(window(), tr("Несохранённая экипировка"),
        tr("Отменить несохранённые изменения экипировки?"), QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) == QMessageBox::Yes;
}

void Tab_Equipment::refresh()
{
    const bool pending = hasDraft();
    populate();
    if (valid && pending)
        ui->equipmentStatus->setText(tr("Контекст изменился. Несохранённая экипировка отменена."));
}

void Tab_Equipment::retranslate()
{
    const auto pending = draft();
    ui->retranslateUi(this);
    const auto &characters = JsonParser::characters();
    if (valid && characterIndex == Tab_Settings::getCharacterId() &&
        characterIndex >= 0 && characterIndex < characters.size() &&
        baseline == equipmentContext(characters.at(characterIndex)))
        populate(pending);
    else
        refresh();
}

void Tab_Equipment::populate(const Equipment::Changes &pending)
{
    characterIndex = Tab_Settings::getCharacterId();
    model = {};
    baseline = {};
    valid = false;
    readError.clear();
    const auto &characters = JsonParser::characters();
    if (inventory && characterIndex >= 0 && characterIndex < characters.size()) {
        const auto &character = characters.at(characterIndex);
        ui->characterLabel->setText(tr("Персонаж %1: %2").arg(characterIndex + 1)
                                      .arg(character.character.value("name").toString()));
        baseline = equipmentContext(character);
        valid = Equipment::read(character, inventory->equipmentCatalog(), model, readError);
    } else {
        ui->characterLabel->setText(tr("Нет персонажа"));
        readError = tr("Выберите персонажа в настройках.");
    }
    for (const auto &binding : bindings) {
        auto *box = binding.box;
        const QSignalBlocker blocker(box);
        box->clear();
        box->setToolTip({});
        if (!valid) continue;
        const auto *stored = model.find(binding.position);
        QList<Equipment::Slot> choices;
        QString error;
        if (!Equipment::choices(characters.at(characterIndex), inventory->equipmentCatalog(),
                                binding.position, choices, error))
            choices = {*stored}; // exact unrecognized reference remains visible
        for (const auto &choice : choices) {
            box->addItem(choiceText(choice), choice.reference);
            box->setItemData(box->count() - 1, choiceDetails(choice), Qt::ToolTipRole);
        }
        QString target = stored->reference;
        for (const auto &selection : pending.selections)
            if (selection.position == binding.position) target = selection.reference;
        int index = box->findData(target);
        if (index < 0) index = box->findData(stored->reference);
        box->setCurrentIndex(index);
    }
    const auto populateIndex = [&](QComboBox *box, Kind kind, int stored, std::optional<int> wanted) {
        const QSignalBlocker blocker(box);
        box->clear();
        if (!valid) return;
        for (int i = 0; i < Equipment::capacity(kind); ++i) {
            const auto *slot = model.find({kind, i});
            box->addItem(slot->editable() ? tr("Ячейка %1").arg(i + 1) :
                tr("Ячейка %1 (только чтение)").arg(i + 1), i);
            box->setItemData(i, choiceDetails(*slot), Qt::ToolTipRole);
            if (!slot->editable())
                if (auto *items = qobject_cast<QStandardItemModel *>(box->model()))
                    items->item(i)->setEnabled(false);
        }
        box->setCurrentIndex(wanted.value_or(stored));
    };
    populateIndex(ui->activeRight, Kind::RightHand, model.rightHandIndex, pending.rightHandIndex);
    populateIndex(ui->activeLeft, Kind::LeftHand, model.leftHandIndex, pending.leftHandIndex);
    populateIndex(ui->activeConsumable, Kind::Consumable, model.consumableIndex, pending.consumableIndex);
    updatePreview();
}

void Tab_Equipment::updatePreview()
{
    const auto changes = draft();
    const bool pending = dirty(changes);
    const auto &characters = JsonParser::characters();
    const bool current = valid && characterIndex == Tab_Settings::getCharacterId() &&
        characterIndex >= 0 && characterIndex < characters.size() &&
        equipmentContext(characters.at(characterIndex)) == baseline;
    const bool available = current && JsonParser::canEdit();
    for (const auto &binding : bindings) {
        const auto *slot = valid ? model.find(binding.position) : nullptr;
        binding.box->setEnabled(available && slot && slot->editable() && binding.box->count() > 1);
        if (slot) {
            QString details = tr("Сохранено: %1").arg(choiceDetails(*slot));
            if (slot->editable() && binding.box->count() <= 1)
                details += "\n" + tr("Нет других проверенных принадлежащих предметов.");
            if (binding.box->currentData().toString() != slot->reference)
                details += "\n" + tr("Выбрано: %1").arg(binding.box->currentData(Qt::ToolTipRole).toString());
            binding.box->setToolTip(details);
        }
    }
    for (auto *box : {ui->activeRight, ui->activeLeft, ui->activeConsumable})
        box->setEnabled(available);
    ui->cancelEquipment->setEnabled(pending);
    ui->applyEquipment->setEnabled(false);
    QString error;
    if (!valid) error = readError;
    else if (!available) error = tr("Редактирование недоступно. Закройте игру, завершите синхронизацию Steam и перезагрузите сохранение.");
    else if (pending) {
        auto candidate = characters.at(characterIndex);
        if (Equipment::apply(candidate, inventory->equipmentCatalog(), changes, error)) {
            ui->applyEquipment->setEnabled(true);
            error = tr("Изменения не сохранены. Примените или отмените.");
        } else {
            error = tr("Недопустимая экипировка: %1").arg(error);
        }
    } else {
        error = tr("Доступны только проверенные принадлежащие предметы. Откройте каталог и проверенный список на вкладке «Сумка» для дополнительных вариантов.");
    }
    ui->equipmentStatus->setText(error);
}

void Tab_Equipment::apply()
{
    const auto changes = draft();
    if (!dirty(changes)) return;
    const auto &characters = JsonParser::characters();
    QString error;
    ItemCatalog::Catalog verified;
    if (!valid || characterIndex != Tab_Settings::getCharacterId() ||
        characterIndex < 0 || characterIndex >= characters.size() ||
        equipmentContext(characters.at(characterIndex)) != baseline || !JsonParser::canEdit()) {
        error = tr("Персонаж или сохранение изменились. Перезагрузите сохранение и повторите.");
    } else if (inventory->verifyEquipmentCatalog(verified, error) &&
               Equipment::commit(characterIndex, verified, changes, error)) {
        populate();
        inventory->refresh();
        ui->equipmentStatus->setText(tr("Экипировка сохранена."));
        return;
    }
    updatePreview();
    ui->equipmentStatus->setText(tr("Сохранение не изменено: %1").arg(error));
}
