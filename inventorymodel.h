#ifndef INVENTORYMODEL_H
#define INVENTORYMODEL_H

#include "savedocument.h"
#include "itemcatalog.h"
#include <QJsonArray>
#include <QStringList>

// Persisted instance identity is (character slot, array kind, instance ID).
// GUID is the case-sensitive definition identifier, not an instance key.
namespace Inventory {
enum class ArrayKind { Generic, Weapon };
enum class LegacyGroup { Spells, Runes };
QString arrayKey(ArrayKind kind);

struct Instance {
    ArrayKind kind;
    QString instanceId;
    QString guid;
    QJsonObject fields; // Complete record, including unknown fields.
};

// Validates both arrays/record shapes without requiring catalog membership.
// Existing non-UUID IDs (e.g. starter items) are valid; duplicate definition
// GUIDs are valid distinct instances. Duplicate IDs within an array are not.
bool read(const SaveCharacter &slot, QList<Instance> &out, QString &error);
bool read(const SaveDocument &doc, int slotIndex, QList<Instance> &out, QString &error);
const Instance *find(const QList<Instance> &instances, ArrayKind kind, const QString &instanceId);

// Pure candidate mutations; call within SaveStore/JsonParser::mutateCharacterChecked
// to publish once, with the existing lock, stale-save & recovery guards.
// On validation failure the character is untouched and added is zero.
// Count is staged, not proof of persistence until the outer commit succeeds.
// New catalog grants use fixture-derived defaults. Disposable-profile game
// qualification is outstanding; DIG-124 owns the new grant UI.
bool addMissing(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
                const QStringList &guids, qsizetype &added, QString &error,
                bool dlcEntitled = false);
bool addOne(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
            const QString &guid, qsizetype &added, QString &error,
            bool dlcEntitled = false);
bool addAllMissing(SaveCharacter &slot, const ItemCatalog::Catalog &catalog,
                   const QList<ItemCatalog::Category> &categories,
                   qsizetype &added, QString &error, bool dlcEntitled = false);

// Existing Stats-only spell/rune allowlists; no arbitrary GUID bypass.
bool addLegacyMissing(SaveCharacter &slot, LegacyGroup group, qsizetype &added, QString &error);
}
#endif
