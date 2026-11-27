#include "inventoryproperties.h"
#include "jsonparser.h"

namespace Inventory {
namespace {

GenericState genericState(const QJsonObject &item)
{
    return {item.value("uses").toInt(), item.value("hasSeen").toBool()};
}
WeaponState weaponState(const QJsonObject &item)
{
    return {item.value("level").toInt(), item.value("infusionType").toInt(),
            item.value("hasSeen").toBool()};
}

// Do not use value.toInt() alone: missing/null/fractional values can look like 0.
bool exactZero(const QJsonValue &value)
{
    return value.isDouble() && value.toDouble() == 0;
}

bool validContext(const SaveCharacter &slot, const Instance &item)
{
    // Restrict the present rules to ordinary, non-remembered/unburdened
    // NG0 records. Never alter or reconcile these companion fields.
    if (item.fields.value("isAwake") != QJsonValue(false) ||
        item.fields.value("isHoovered") != QJsonValue(false) ||
        !exactZero(item.fields.value("newGamePlusCount")) ||
        !exactZero(slot.character.value("newGamePlusCount")))
        return false;
    for (const auto *key : {"rememberedGUIDs", "hooveredGUIDs"}) {
        const auto value = slot.character.value(QLatin1String(key));
        if (!value.isArray()) return false;
        for (const auto &guid : value.toArray()) {
            if (!guid.isString() || guid.toString() == item.guid) return false;
        }
    }
    return true;
}

bool validRule(const PropertyRule &rule)
{
    if (rule.guid.isEmpty() || rule.evidence.trimmed().isEmpty()) return false;
    if (rule.kind == ArrayKind::Generic) {
        if (rule.genericStates.isEmpty() || !rule.weaponStates.isEmpty()) return false;
        // A -1 sentinel is observed on selected non-consumables; never offer
        // a conversion between finite counts and that different item mode.
        const bool sentinel = rule.genericStates.first().uses == -1;
        for (const auto &state : rule.genericStates)
            if (state.uses < -1 || (state.uses == -1) != sentinel) return false;
        return true;
    }
    if (rule.kind != ArrayKind::Weapon || rule.weaponStates.isEmpty() ||
        !rule.genericStates.isEmpty()) return false;
    for (const auto &state : rule.weaponStates)
        if (state.level < 0 || state.infusionType < 0) return false;
    return true;
}

} // namespace

const QList<PropertyRule> &propertyRules()
{
    // Conservative fixture-backed candidate states: exact existing tuples,
    // not the entire interval or every cross-product of recorded values.
    // DIG-129 still needs to qualify actual game reload/behavior.
    static const QList<PropertyRule> rules {
        {ArrayKind::Generic, "Consumable_SlowStaminaRestore",
         {{1, false}, {2, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        {ArrayKind::Generic, "Consumable_SneakAid",
         {{8, false}, {9, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        {ArrayKind::Generic, "Consumable_WepStatus_Glitch",
         {{5, false}, {6, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        {ArrayKind::Generic, "Consumable_StrongPotion_Health",
         {{1, false}, {8, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        {ArrayKind::Generic, "GenericAmmo",
         {{10, true}, {20, true}}, {}, "SaveData1.txt / SaveData.txt, NG0"},
        {ArrayKind::Generic, "MessageSign",
         {{-1, false}, {-1, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        {ArrayKind::Generic, "SkeletonArmor_Helm",
         {{-1, false}, {-1, true}}, {}, "SaveData.txt: Test / DeathWhisper, NG0"},
        // Both seen values with the *same* ordinary weapon state.
        {ArrayKind::Weapon, "PlotThreadScissors_Dagger", {},
         {{0, 0, false}, {0, 0, true}}, "SaveData.txt: Test / DeathWhisper, NG0"}
    };
    return rules;
}

bool selectProperties(const QList<SaveCharacter> &characters, int slotIndex,
                      ArrayKind kind, const QString &instanceId,
                      PropertySelection &selection, QString &error)
{
    selection = {};
    if (slotIndex < 0 || slotIndex >= characters.size()) {
        error = "No valid selected character.";
        return false;
    }
    const auto &slot = characters.at(slotIndex);
    const QString guid = slot.character.value("guid").toString();
    if (guid.trimmed().isEmpty()) {
        error = "Selected character has no stable GUID.";
        return false;
    }
    QList<Instance> instances;
    if (!read(slot, instances, error)) return false;
    const auto *item = find(instances, kind, instanceId);
    if (!item) {
        error = "Selected inventory instance does not exist.";
        return false;
    }
    selection = {slotIndex, guid, *item};
    error.clear();
    return true;
}

PropertyOptions propertyOptions(const SaveCharacter &slot, const Instance &item,
                                const QList<PropertyRule> &rules)
{
    PropertyOptions result;
    result.usesReadOnlyReason = "No qualified alternative count or sentinel state.";
    result.hasSeenReadOnlyReason = "No qualified alternative seen state.";
    result.levelReadOnlyReason = "No qualified alternative upgrade level.";
    result.infusionReadOnlyReason = "No qualified alternative infusion for this definition.";
    result.flagsReadOnlyReason =
        "Remembering/unburdening flags may affect character lists and counters; transitions are unverified.";
    result.ngPlusReadOnlyReason = "Item NG+ semantics are unverified.";
    // Resolve against current data rather than trusting an arbitrary Instance.
    QList<Instance> instances;
    QString error;
    if (!read(slot, instances, error)) {
        result.readOnlyReason = error;
        return result;
    }
    const auto *current = find(instances, item.kind, item.instanceId);
    if (!current || current->guid != item.guid || current->fields != item.fields) {
        result.readOnlyReason = "Selected inventory instance changed; refresh the selection.";
        return result;
    }
    const PropertyRule *match = nullptr;
    for (const auto &rule : rules) {
        if (rule.kind != item.kind || rule.guid != item.guid) continue;
        if (match || !validRule(rule)) {
            result.readOnlyReason = "Ambiguous or invalid property review.";
            return result;
        }
        match = &rule;
    }
    if (!match) {
        result.readOnlyReason = "No reviewed property states for this definition and array.";
        return result;
    }
    if (!validContext(slot, item)) {
        result.readOnlyReason = "Remembered, unburdened, NG+ or inconsistent companion state is unqualified.";
        return result;
    }
    if (item.kind == ArrayKind::Generic && match->genericStates.contains(genericState(item.fields))) {
        result.genericStates = match->genericStates;
        const auto actual = genericState(item.fields);
        for (const auto &state : result.genericStates) {
            if (state.uses != actual.uses) result.usesReadOnlyReason.clear();
            if (state.hasSeen != actual.hasSeen) result.hasSeenReadOnlyReason.clear();
        }
    } else if (item.kind == ArrayKind::Weapon && match->weaponStates.contains(weaponState(item.fields))) {
        result.weaponStates = match->weaponStates;
        const auto actual = weaponState(item.fields);
        for (const auto &state : result.weaponStates) {
            if (state.level != actual.level) result.levelReadOnlyReason.clear();
            if (state.infusionType != actual.infusionType) result.infusionReadOnlyReason.clear();
            if (state.hasSeen != actual.hasSeen) result.hasSeenReadOnlyReason.clear();
        }
    } else {
        result.readOnlyReason = "Current property values are outside the reviewed states.";
        return result;
    }
    result.evidence = match->evidence;
    return result;
}

bool mutateProperties(SaveCharacter &slot, int currentSlotIndex,
                      const PropertySelection &selection, const PropertyEdits &edits,
                      QString &error, const QList<PropertyRule> &rules)
{
    if (currentSlotIndex < 0 || selection.slotIndex != currentSlotIndex ||
        selection.characterGuid.isEmpty() ||
        slot.character.value("guid").toString() != selection.characterGuid) {
        error = "Selected character changed; refresh the inventory.";
        return false;
    }
    const auto &item = selection.item;
    const auto options = propertyOptions(slot, item, rules);
    if (!options.readOnlyReason.isEmpty()) {
        error = options.readOnlyReason;
        return false;
    }
    auto updated = item.fields;
    if (item.kind == ArrayKind::Generic) {
        if (edits.level || edits.infusionType) {
            error = "Weapon properties cannot be edited on a generic item.";
            return false;
        }
        auto state = genericState(item.fields);
        if (edits.uses) state.uses = *edits.uses;
        if (edits.hasSeen) state.hasSeen = *edits.hasSeen;
        if (!options.genericStates.contains(state)) {
            error = "Uses/seen combination is not a reviewed state for this item.";
            return false;
        }
        updated.insert("uses", state.uses);
        updated.insert("hasSeen", state.hasSeen);
    } else {
        if (edits.uses) {
            error = "Uses cannot be edited on a weapon or shield.";
            return false;
        }
        auto state = weaponState(item.fields);
        if (edits.level) state.level = *edits.level;
        if (edits.infusionType) state.infusionType = *edits.infusionType;
        if (edits.hasSeen) state.hasSeen = *edits.hasSeen;
        if (!options.weaponStates.contains(state)) {
            error = "Upgrade/infusion/seen combination is not a reviewed state for this item.";
            return false;
        }
        updated.insert("level", state.level);
        updated.insert("infusionType", state.infusionType);
        updated.insert("hasSeen", state.hasSeen);
    }
    // Only replace the exact matching record, without moving it or rewriting
    // companion arrays or references. A no-op leaves the candidate untouched.
    if (updated != item.fields) {
        const auto key = arrayKey(item.kind);
        auto array = slot.character.value(key).toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (array.at(i).toObject().value("instanceIDOnCreation").toString() == item.instanceId) {
                array.replace(i, updated);
                slot.character.insert(key, array);
                error.clear();
                return true;
            }
        }
        error = "Selected inventory instance no longer exists.";
        return false;
    }
    error.clear();
    return true;
}

bool applyProperties(int currentSlotIndex, const PropertySelection &selection,
                     const PropertyEdits &edits, QString &error,
                     const QList<PropertyRule> &rules)
{
    return JsonParser::mutateCharacterChecked(currentSlotIndex,
        [&](SaveCharacter &slot, QString &mutationError) {
            return mutateProperties(slot, currentSlotIndex, selection, edits, mutationError, rules);
        }, error);
}
} // namespace Inventory
