#ifndef EQUIPMENTSERVICE_H
#define EQUIPMENTSERVICE_H

#include "inventorymodel.h"
#include <optional>

namespace Equipment {
enum class Kind { Helm, Torso, Arms, Legs, RightHand, LeftHand, Ring, Consumable };
enum class Status { Empty, Resolved, Unverified, Dangling, Invalid };

struct Position {
    Kind kind;
    int index = 0; // 0 for scalar armor fields; zero-based for arrays
    bool operator==(const Position &other) const { return kind == other.kind && index == other.index; }
};

struct Slot {
    Position position;
    QString reference; // exact persisted ID/GUID/sentinel
    QString guid; // definition identity for display, not an instance ID
    QString displayName;
    Status status;
    QString diagnostic;
    bool editable() const { return status == Status::Empty || status == Status::Resolved; }
};

struct Model {
    QList<Slot> entries;
    int rightHandIndex = 0;
    int leftHandIndex = 0;
    int consumableIndex = 0;
    const Slot *find(Position position) const;
};

struct Selection {
    Position position;
    QString reference;
};

// Omitted fields stay untouched. An empty sentinel is selected explicitly.
// All references and indices are validated together against the final state.
struct Changes {
    QList<Selection> selections;
    std::optional<int> rightHandIndex;
    std::optional<int> leftHandIndex;
    std::optional<int> consumableIndex;
};

QString field(Kind kind);
QString emptyReference(Kind kind);
int capacity(Kind kind);

// False for unsupported structural shape or invalid inventory/index.
// For a supported shape, unknown/dangling refs appear in entries with a
// diagnostic and are read-only; reading never changes the character.
bool read(const SaveCharacter &character, const ItemCatalog::Catalog &catalog,
          Model &out, QString &error);

// Eligible owned choices + the sentinel. Distinct instance IDs survive
// duplicate GUIDs. Ring choices use a definition GUID. Other occupied slots
// are not filtered, to allow atomic swaps; apply checks final uniqueness.
bool choices(const SaveCharacter &character, const ItemCatalog::Catalog &catalog,
             Position position, QList<Slot> &out, QString &error);

// Pure checked candidate mutation; no partial field updates on failure.
bool apply(SaveCharacter &character, const ItemCatalog::Catalog &catalog,
           const Changes &changes, QString &error);

// Normal application entry point: one protected selected-character write.
bool commit(int characterIndex, const ItemCatalog::Catalog &catalog,
            const Changes &changes, QString &error);
}
#endif
