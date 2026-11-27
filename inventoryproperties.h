#ifndef INVENTORYPROPERTIES_H
#define INVENTORYPROPERTIES_H

#include "inventorymodel.h"
#include <optional>

namespace Inventory {

// Explicitly reviewed complete states, not independent guessed maxima.
// Rules are trusted application code, never derived from a save or a UI edit.
struct GenericState {
    int uses;
    bool hasSeen;
    bool operator==(const GenericState &other) const {
        return uses == other.uses && hasSeen == other.hasSeen;
    }
};
struct WeaponState {
    int level;
    int infusionType;
    bool hasSeen;
    bool operator==(const WeaponState &other) const {
        return level == other.level && infusionType == other.infusionType &&
               hasSeen == other.hasSeen;
    }
};
struct PropertyRule {
    ArrayKind kind;
    QString guid;
    QList<GenericState> genericStates;
    QList<WeaponState> weaponStates;
    QString evidence;
};

// Production rules intentionally narrow. No nonzero weapon infusion or upgrade
// is currently qualified; do not infer rules from a catalog/path or save.
const QList<PropertyRule> &propertyRules();

struct PropertyOptions {
    QList<GenericState> genericStates;
    QList<WeaponState> weaponStates;
    QString evidence;
    QString readOnlyReason;
    QString usesReadOnlyReason;
    QString hasSeenReadOnlyReason;
    QString levelReadOnlyReason;
    QString infusionReadOnlyReason;
    // These fields never have an editable control. Their consistency with
    // achievement/remembering/hoovering state has not been established.
    QString flagsReadOnlyReason;
    QString ngPlusReadOnlyReason;
};

struct PropertySelection {
    int slotIndex = -1;
    QString characterGuid;
    Instance item{};
};

// Capture from the current app-selected character. The complete record is
// retained for optimistic selection checks, not just the instance ID.
bool selectProperties(const QList<SaveCharacter> &characters, int slotIndex,
                      ArrayKind kind, const QString &instanceId,
                      PropertySelection &selection, QString &error);
PropertyOptions propertyOptions(const SaveCharacter &slot, const Instance &item,
                                const QList<PropertyRule> &rules = propertyRules());

// A typed, partial batch; identity, coupled flags, NG+ and unknown fields
// cannot be requested through this API. The resulting complete state must be
// one reviewed state (including the untouched partner fields).
struct PropertyEdits {
    std::optional<int> uses;
    std::optional<bool> hasSeen;
    std::optional<int> level;
    std::optional<int> infusionType;
};

// Candidate-only operation. Prefer the checked app entry point below. The
// selected index must be the CURRENT index, not merely the captured one.
bool mutateProperties(SaveCharacter &slot, int currentSlotIndex,
                      const PropertySelection &selection, const PropertyEdits &edits,
                      QString &error, const QList<PropertyRule> &rules = propertyRules());

bool applyProperties(int currentSlotIndex, const PropertySelection &selection,
                     const PropertyEdits &edits, QString &error,
                     const QList<PropertyRule> &rules = propertyRules());
}
#endif
