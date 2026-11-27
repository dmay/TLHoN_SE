#include "inventorymodel.h"

#include <QSet>
#include <QUuid>
#include <cmath>
#include <limits>

namespace Inventory {
QString arrayKey(ArrayKind kind)
{
    return kind == ArrayKind::Generic ? QStringLiteral("inventory") : QStringLiteral("inventoryWeapons");
}

namespace {
bool integer(const QJsonValue &value)
{
    if (!value.isDouble()) return false;
    const double n = value.toDouble();
    return std::isfinite(n) && n >= std::numeric_limits<int>::min() &&
           n <= std::numeric_limits<int>::max() && std::floor(n) == n;
}

bool validRecord(const QJsonObject &record, ArrayKind kind)
{
    if (record.value("GUID").toString().trimmed().isEmpty() ||
        record.value("instanceIDOnCreation").toString().trimmed().isEmpty())
        return false;
    for (const auto *key : {"isAwake", "isHoovered", "hasSeen"})
        if (!record.value(QLatin1String(key)).isBool()) return false;
    if (!integer(record.value("newGamePlusCount"))) return false;
    return kind == ArrayKind::Generic ? integer(record.value("uses"))
        : integer(record.value("level")) && integer(record.value("infusionType"));
}

QJsonObject createRecord(ArrayKind kind, const QString &guid, const QString &id)
{
    // Candidate baseline observed in unupgraded/ordinary records in both
    // repository fixtures. A disposable game-created item comparison is
    // still outstanding; no new grant controls are wired in DIG-122.
    QJsonObject item {
        {"GUID", guid}, {"instanceIDOnCreation", id},
        {"isAwake", false}, {"isHoovered", false}, {"hasSeen", true},
        {"newGamePlusCount", 0}
    };
    if (kind == ArrayKind::Generic) item.insert("uses", -1);
    else {
        item.insert("level", 0);
        item.insert("infusionType", 0);
    }
    return item;
}

struct Grant {
    ArrayKind kind;
    QString guid;
};

// Construct the complete result on copies. Only publish the arrays that
// changed, after all grant validation and exact GUID deduplication.
bool appendMissing(SaveCharacter &slot, const QList<Grant> &grants,
                   qsizetype &added, QString &error)
{
    added = 0;
    QList<Instance> instances;
    if (!read(slot, instances, error)) return false;

    QSet<QString> ids, genericGuids, weaponGuids;
    for (const auto &item : instances) {
        ids.insert(item.instanceId);
        (item.kind == ArrayKind::Generic ? genericGuids : weaponGuids).insert(item.guid);
    }
    QJsonArray generic = slot.character.value(arrayKey(ArrayKind::Generic)).toArray();
    QJsonArray weapons = slot.character.value(arrayKey(ArrayKind::Weapon)).toArray();
    qsizetype count = 0;
    for (const auto &grant : grants) {
        auto &guids = grant.kind == ArrayKind::Generic ? genericGuids : weaponGuids;
        if (guids.contains(grant.guid)) continue;
        QString id;
        do {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        } while (ids.contains(id));
        ids.insert(id);
        auto &array = grant.kind == ArrayKind::Generic ? generic : weapons;
        array.append(createRecord(grant.kind, grant.guid, id));
        guids.insert(grant.guid);
        ++count;
    }

    if (count != 0) {
        if (generic != slot.character.value(arrayKey(ArrayKind::Generic)).toArray())
            slot.character.insert(arrayKey(ArrayKind::Generic), generic);
        if (weapons != slot.character.value(arrayKey(ArrayKind::Weapon)).toArray())
            slot.character.insert(arrayKey(ArrayKind::Weapon), weapons);
    }
    added = count;
    error.clear();
    return true;
}

bool supported(ItemCatalog::Category category)
{
    using Category = ItemCatalog::Category;
    return category == Category::Armor || category == Category::Weapon || category == Category::Shield;
}
const QStringList spells =
    {
        "Spell_WepInfusion_StealHealth",
        "Spell_BombCode",
        "Spell_ThrowablePixelStorm",
        "Spell_RecursionDamage",
        "Spell_KeepMemoryOnDeath",
        "Spell_SpawnDecoy",
        "Spell_TrapTarget",
        "Spell_EscapePath",
        "Spell_WepInfusion_Glitch",
        "Spell_StunSurrounding",
        "Spell_StartingMagicMissile",
        "Spell_ProjectileDOT",
        "Spell_HOT",
        "Spell_SlowPixelBomb",
        "Spell_LightningChain",
        "Spell_Thorns",
        "Throwable_FadedSkuriken"
    };
const QStringList runes =
    {
        "Relic_DecreaseProjectileStagger",
        "Relic_RollIFramesUp",
        "Relic_StatUp_Strength",
        "Relic_PoiseUp",
        "Relic_MemoryDropIncrease",
        "Relic_CritChanceUp",
        "Relic_StaminaRegen",
        "Relic_MagicDefenseUp",
        "Relic_StatUp_Dex",
        "Relic_KiteParryBoost",
        "Relic_SwordParryBoost",
        "Relic_StatUp_Mag",
        "Relic_MagicThorns",
        "Relic_Damage_Abominations",
        "Relic_Damage_Beasts",
        "Relic_Damage_Evil",
        "Relic_Damage_Humanoids",
        "Relic_HealthRegenOnCrit",
        "Relic_DecreaseStaminaUsage",
        "Relic_IncreaseHeroicIconPotency",
        "Relic_AmmoCapacityUp",
        "Relic_RecursionDamageUp",
        "Relic_EquipLoadUp",
        "Relic_ReduceBuildUp",
        "Relic_HealthBelowAtkBoost",
    };
} // namespace

bool read(const SaveCharacter &slot, QList<Instance> &out, QString &error)
{
    out.clear();
    QList<Instance> result;
    for (const auto kind : {ArrayKind::Generic, ArrayKind::Weapon}) {
        const auto key = arrayKey(kind);
        const auto value = slot.character.value(key);
        if (!value.isArray()) {
            error = QString("Expected array '%1' in selected character.").arg(key);
            return false;
        }
        const auto array = value.toArray();
        QSet<QString> ids;
        for (qsizetype i = 0; i < array.size(); ++i) {
            const auto record = array.at(i).toObject();
            if (!validRecord(record, kind)) {
                error = QString("Invalid item record in %1 at index %2.").arg(key).arg(i);
                return false;
            }
            const QString id = record.value("instanceIDOnCreation").toString();
            if (ids.contains(id)) {
                error = QString("Ambiguous instance ID '%1' in %2.").arg(id, key);
                return false;
            }
            ids.insert(id);
            result.append(Instance{kind, id, record.value("GUID").toString(), record});
        }
    }
    out.swap(result);
    error.clear();
    return true;
}

bool read(const SaveDocument &doc, int slotIndex, QList<Instance> &out, QString &error)
{
    if (!doc.hasCharacter(slotIndex)) {
        out.clear();
        error = "No valid selected character.";
        return false;
    }
    return read(doc.characters().at(slotIndex), out, error);
}

const Instance *find(const QList<Instance> &instances, ArrayKind kind, const QString &instanceId)
{
    const Instance *found = nullptr;
    for (const auto &item : instances) {
        if (item.kind == kind && item.instanceId == instanceId) {
            if (found) return nullptr;
            found = &item;
        }
    }
    return found;
}

bool addMissing(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
                const QStringList &guids, qsizetype &added, QString &error, bool dlcEntitled)
{
    added = 0;
    QList<Grant> grants;
    // Validate the entire requested set before touching either inventory.
    for (const auto &guid : guids) {
        const auto *entry = catalog.find(guid);
        if (guid.isEmpty() || !entry || entry->guid != guid ||
            !supported(entry->category) || !entry->bulkEligible(dlcEntitled)) {
            error = QString("Item '%1' is unknown, unsupported or not eligible in this catalog.").arg(guid);
            return false;
        }
        grants.append(Grant{entry->category == ItemCatalog::Category::Armor
            ? ArrayKind::Generic : ArrayKind::Weapon, guid});
    }
    return appendMissing(slot, grants, added, error);
}

bool addOne(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
            const QString &guid, qsizetype &added, QString &error, bool dlcEntitled)
{
    return addMissing(slot, catalog, {guid}, added, error, dlcEntitled);
}

bool addAllMissing(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
                   const QList<ItemCatalog::Category> &categories,
                   qsizetype &added, QString &error, bool dlcEntitled)
{
    added = 0;
    QStringList guids;
    for (const auto category : categories) {
        if (!supported(category)) {
            error = "Unsupported catalog category for item grants.";
            return false;
        }
        for (const auto &entry : catalog.bulkEligible(category, dlcEntitled))
            guids.append(entry.guid);
    }
    if (guids.isEmpty()) {
        error = "No reviewed, eligible definitions in the selected categories.";
        return false;
    }
    return addMissing(slot, catalog, guids, added, error, dlcEntitled);
}

bool addLegacyMissing(SaveCharacter &slot, LegacyGroup group, qsizetype &added, QString &error)
{
    QList<Grant> grants;
    for (const auto &guid : group == LegacyGroup::Spells ? spells : runes)
        grants.append(Grant{ArrayKind::Generic, guid});
    return appendMissing(slot, grants, added, error);
}
} // namespace Inventory
