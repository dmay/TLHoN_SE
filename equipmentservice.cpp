#include "equipmentservice.h"
#include "jsonparser.h"

#include <QSet>
#include <cmath>

namespace Equipment {
namespace {
const QList<Kind> kinds{Kind::Helm, Kind::Torso, Kind::Arms, Kind::Legs,
                        Kind::RightHand, Kind::LeftHand, Kind::Ring, Kind::Consumable};
bool isHand(Kind kind) { return kind == Kind::RightHand || kind == Kind::LeftHand; }
bool isArmor(Kind kind)
{
    return kind == Kind::Helm || kind == Kind::Torso || kind == Kind::Arms || kind == Kind::Legs;
}
bool valid(Position position)
{
    return kinds.contains(position.kind) && position.index >= 0 && position.index < capacity(position.kind);
}

QString slotName(Kind kind)
{
    switch (kind) {
    case Kind::Helm: return "helm";
    case Kind::Torso: return "torso";
    case Kind::Arms: return "arms";
    case Kind::Legs: return "legs";
    case Kind::RightHand: return "rightHand";
    case Kind::LeftHand: return "leftHand";
    case Kind::Ring: return "ring";
    case Kind::Consumable: return "consumable";
    }
    return {};
}

// Exact definition/slot pairs seen equipped in the committed fixtures.
// Do not infer from a name suffix, an unreviewed catalog path, or an
// arbitrary save's current contents. More types need reviewed metadata.
const QMap<QString, Kind> observed{
    {"Starting_Knight_Helm", Kind::Helm}, {"Starting_Knight_Torso", Kind::Torso},
    {"Starting_Knight_Arms", Kind::Arms}, {"Starting_Knight_Legs", Kind::Legs},
    {"MasterChefArmor_Helm", Kind::Helm}, {"MasterChefArmor_Torso", Kind::Torso},
    {"MasterChefArmor_Arms", Kind::Arms}, {"MasterChefArmor_Legs", Kind::Legs},
    {"AvatarBaseSword", Kind::RightHand}, {"AvatarBaseShield", Kind::LeftHand},
    {"OldWoodenCultShield_StandardShield", Kind::LeftHand},
    {"Torch", Kind::LeftHand}, {"HealthPotion", Kind::Consumable}
};

bool matchesCategory(Kind kind, ItemCatalog::Category category)
{
    using Category = ItemCatalog::Category;
    if (isArmor(kind)) return category == Category::Armor;
    if (isHand(kind)) return category == Category::Weapon || category == Category::Shield;
    return category == Category::Other;
}

void diagnose(Slot &slot, Status status, const QString &message)
{
    slot.status = status;
    slot.diagnostic = QString("%1[%2]: %3").arg(field(slot.position.kind))
        .arg(slot.position.index).arg(message);
}

Slot resolve(Position position, const QString &reference, const QList<Inventory::Instance> &items,
             const ItemCatalog::Catalog &catalog)
{
    Slot slot{position, reference, {}, reference, Status::Unverified, {}};
    if (reference == emptyReference(position.kind)) {
        slot.status = Status::Empty;
        return slot;
    }
    if (reference == "NULL" || reference == "Base_Punch" || reference.trimmed().isEmpty()) {
        diagnose(slot, Status::Invalid, "Invalid sentinel or empty reference.");
        return slot;
    }

    // Rings use a definition GUID; multiple owned copies of the same
    // definition are not ambiguous. All other slots use the expected array ID.
    const auto array = isHand(position.kind) ? Inventory::ArrayKind::Weapon : Inventory::ArrayKind::Generic;
    const Inventory::Instance *item = nullptr;
    if (position.kind == Kind::Ring) {
        for (const auto &entry : items)
            if (entry.kind == array && entry.guid == reference) { item = &entry; break; }
    } else {
        item = Inventory::find(items, array, reference);
    }
    if (!item) {
        diagnose(slot, Status::Dangling, "Reference does not resolve to an owned item in the expected array.");
        return slot;
    }

    slot.guid = item->guid;
    slot.displayName = item->guid;
    const auto *definition = catalog.find(item->guid);
    const auto known = observed.constFind(item->guid);
    if (definition) {
        if (definition->displayNameVerified) slot.displayName = definition->displayName;
        if (definition->availability == ItemCatalog::Availability::Excluded ||
            definition->sourcePaths.size() != 1) {
            diagnose(slot, Status::Unverified, "Catalog definition is excluded or ambiguous.");
            return slot;
        }
        if (!matchesCategory(position.kind, definition->category)) {
            diagnose(slot, Status::Invalid, "Catalog category does not match this slot.");
            return slot;
        }
        if (!definition->equipmentSlots.isEmpty() && !definition->evidence.trimmed().isEmpty()) {
            if (definition->equipmentSlots.contains(slotName(position.kind))) {
                slot.status = Status::Resolved;
            } else {
                diagnose(slot, Status::Invalid, "Reviewed definition is not eligible for this slot.");
            }
            return slot;
        }
    }
    if (known != observed.constEnd() && known.value() == position.kind) {
        slot.status = Status::Resolved;
    } else {
        diagnose(slot, Status::Unverified, "No verified eligibility for this definition and slot.");
    }
    return slot;
}

// Same stored instance cannot occupy two slots. Ring identity is a definition
// GUID, so repeated ring definitions are conservatively rejected. Never treat
// repeated empty sentinels as duplicates. Array kinds scope instance IDs.
QString uniquenessKey(const Slot &slot)
{
    if (slot.status == Status::Empty || slot.reference.isEmpty()) return {};
    const auto kind = slot.position.kind;
    return (kind == Kind::Ring ? QString("ring:") : isHand(kind) ? QString("weapon:") : QString("generic:"))
           + slot.reference;
}

void flagDuplicates(Model &model)
{
    QMap<QString, int> counts;
    for (const auto &entry : model.entries) {
        const auto key = uniquenessKey(entry);
        if (!key.isEmpty()) ++counts[key];
    }
    for (auto &entry : model.entries) {
        if (counts.value(uniquenessKey(entry)) > 1)
            diagnose(entry, Status::Invalid, "Duplicate equipment reference.");
    }
}

bool readIndex(const QJsonObject &character, const QString &key, int count, int &out, QString &error)
{
    const auto value = character.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number >= count ||
        std::floor(number) != number) {
        error = QString("Invalid active index '%1'; expected integer 0..%2.").arg(key).arg(count - 1);
        return false;
    }
    out = static_cast<int>(number);
    return true;
}

QJsonValue referenceAt(const QJsonObject &character, Position position)
{
    const auto value = character.value(field(position.kind));
    return isArmor(position.kind) ? value : value.toArray().at(position.index);
}

void setReference(QJsonObject &character, Position position, const QString &reference)
{
    const auto key = field(position.kind);
    if (isArmor(position.kind)) {
        character.insert(key, reference);
    } else {
        auto array = character.value(key).toArray();
        array.replace(position.index, reference);
        character.insert(key, array);
    }
}
} // namespace

QString field(Kind kind)
{
    switch (kind) {
    case Kind::Helm: return "equipedHelm";
    case Kind::Torso: return "equipedTorso";
    case Kind::Arms: return "equipedArms";
    case Kind::Legs: return "equipedLegs";
    case Kind::RightHand: return "slottedRightHands";
    case Kind::LeftHand: return "slottedLeftHands";
    case Kind::Ring: return "equipedRings";
    case Kind::Consumable: return "equipedConsumables";
    }
    return {};
}

QString emptyReference(Kind kind) { return isHand(kind) ? "Base_Punch" : "NULL"; }

int capacity(Kind kind)
{
    if (isArmor(kind)) return 1;
    if (isHand(kind) || kind == Kind::Ring) return 2;
    if (kind == Kind::Consumable) return 5;
    return 0;
}

const Slot *Model::find(Position position) const
{
    for (const auto &entry : entries)
        if (entry.position == position) return &entry;
    return nullptr;
}

bool read(const SaveCharacter &character, const ItemCatalog::Catalog &catalog,
          Model &out, QString &error)
{
    out = {};
    QList<Inventory::Instance> items;
    if (!Inventory::read(character, items, error)) return false;
    Model model;
    const auto &object = character.character;
    for (const auto kind : kinds) {
        const auto value = object.value(field(kind));
        if (!isArmor(kind) && (!value.isArray() || value.toArray().size() != capacity(kind))) {
            error = QString("Unsupported equipment array '%1'; expected %2 entries.").arg(field(kind)).arg(capacity(kind));
            return false;
        }
        for (int index = 0; index < capacity(kind); ++index) {
            auto reference = referenceAt(object, {kind, index});
            if (!reference.isString()) {
                error = QString("Invalid equipment reference in '%1' at %2; expected string.").arg(field(kind)).arg(index);
                return false;
            }
            model.entries.append(resolve({kind, index}, reference.toString(), items, catalog));
        }
    }
    if (!readIndex(object, "rightHandEquipIndex", capacity(Kind::RightHand), model.rightHandIndex, error) ||
        !readIndex(object, "leftHandEquipIndex", capacity(Kind::LeftHand), model.leftHandIndex, error) ||
        !readIndex(object, "selectedConsumableIndex", capacity(Kind::Consumable), model.consumableIndex, error))
        return false;
    flagDuplicates(model);
    out = model;
    error.clear();
    return true;
}

bool choices(const SaveCharacter &character, const ItemCatalog::Catalog &catalog,
             Position position, QList<Slot> &out, QString &error)
{
    out.clear();
    if (!valid(position)) { error = "Invalid equipment position."; return false; }
    Model model;
    if (!read(character, catalog, model, error)) return false;
    const auto *current = model.find(position);
    if (!current->editable()) { error = current->diagnostic; return false; }
    QList<Inventory::Instance> items;
    if (!Inventory::read(character, items, error)) return false;
    const auto array = isHand(position.kind) ? Inventory::ArrayKind::Weapon : Inventory::ArrayKind::Generic;
    const auto sentinel = emptyReference(position.kind);
    out.append(resolve(position, sentinel, items, catalog));
    QSet<QString> seen{sentinel};
    for (const auto &item : items) {
        if (item.kind != array) continue;
        const auto reference = position.kind == Kind::Ring ? item.guid : item.instanceId;
        if (seen.contains(reference)) continue;
        seen.insert(reference);
        const auto candidate = resolve(position, reference, items, catalog);
        if (candidate.status == Status::Resolved) out.append(candidate);
    }
    error.clear();
    return true;
}

bool apply(SaveCharacter &character, const ItemCatalog::Catalog &catalog,
           const Changes &changes, QString &error)
{
    Model before;
    if (!read(character, catalog, before, error)) return false;
    SaveCharacter candidate = character;
    QList<Position> touched, changed;
    for (const auto &selection : changes.selections) {
        if (!valid(selection.position) || touched.contains(selection.position)) {
            error = "Invalid or repeated equipment position in requested changes.";
            return false;
        }
        touched.append(selection.position);
        const auto *current = before.find(selection.position);
        if (!current->editable()) { error = current->diagnostic; return false; }
        if (current->reference != selection.reference) {
            changed.append(selection.position);
            setReference(candidate.character, selection.position, selection.reference);
        }
    }
    struct ActiveChange { Kind kind; QString key; std::optional<int> index; };
    const QList<ActiveChange> indices{
        {Kind::RightHand, "rightHandEquipIndex", changes.rightHandIndex},
        {Kind::LeftHand, "leftHandEquipIndex", changes.leftHandIndex},
        {Kind::Consumable, "selectedConsumableIndex", changes.consumableIndex}
    };
    for (const auto &active : indices) {
        if (!active.index) continue;
        if (!valid({active.kind, *active.index})) {
            error = QString("Invalid requested active index '%1'.").arg(active.key);
            return false;
        }
        candidate.character.insert(active.key, *active.index);
    }

    Model after;
    if (!read(candidate, catalog, after, error)) return false;
    for (const auto &position : changed) {
        const auto *entry = after.find(position);
        if (!entry->editable()) { error = entry->diagnostic; return false; }
    }
    for (const auto &active : indices) {
        if (active.index) {
            const auto *entry = after.find({active.kind, *active.index});
            if (!entry->editable()) { error = entry->diagnostic; return false; }
        }
    }
    // Empty active slots are explicitly observed; unequipping never silently
    // moves an active index or changes a different slot/init GUID.
    character.character = candidate.character;
    error.clear();
    return true;
}

bool commit(int characterIndex, const ItemCatalog::Catalog &catalog,
            const Changes &changes, QString &error)
{
    return JsonParser::mutateCharacterChecked(characterIndex,
        [&](SaveCharacter &candidate, QString &mutationError) {
            return apply(candidate, catalog, changes, mutationError);
        }, error);
}
} // namespace Equipment
