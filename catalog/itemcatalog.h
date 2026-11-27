#ifndef ITEMCATALOG_H
#define ITEMCATALOG_H

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Read-only definition metadata. GUIDs are exact, case-sensitive save definition
// identifiers, NOT per-instance IDs. Path classification is not proof of obtainability.
namespace ItemCatalog {

enum class Category { Armor, Weapon, Shield, Other };
enum class Availability { Unresolved, Excluded, Validated };
enum class Dlc { Unknown, DlcPath, Base, RequiresDlc };

struct Entry {
    QString guid;
    QStringList sourcePaths;
    Category category = Category::Other;
    QString type; // Path-derived hint unless typeVerified.
    bool typeVerified = false;
    QString slot; // Empty until verified (helm, torso, arms, legs).
    QStringList equipmentSlots; // Explicit reviewed eligibility; no path-based guesses.
    QString displayName; // Exact GUID fallback, not a guessed localized name.
    bool displayNameVerified = false;
    Dlc dlc = Dlc::Unknown;
    Availability availability = Availability::Unresolved;
    QString reason;
    QString evidence;

    // Caller must explicitly establish DLC entitlement and validate inventory
    // factory semantics independently. Never use an unresolved definition.
    bool bulkEligible(bool dlcEntitled = false) const;
};

struct Source {
    QString catalogPath;
    QString steamBuildId; // Empty when not established by a matching manifest.
};

struct Catalog {
    QString sha256;
    QString steamBuildId;
    QString locatorId;
    QString addressablesBuildHash;
    int internalIdCount = 0;
    int ignoredPathCount = 0;
    int repeatedPathCount = 0;
    QMap<QString, Entry> entries;

    const Entry *find(const QString &guid) const;
    QList<Entry> bulkEligible(Category category, bool dlcEntitled = false) const;
    QJsonObject report() const;
};

// On failure, return false, set error, and leave the output empty.
// An explicit game directory wins over Steam discovery; no fallback if invalid.
bool locate(Source &out, QString &error, const QString &gameDir = {},
            const QString &steamRoot = {});
// reviewPath is optional: without it, all candidates are unresolved/excluded.
// A provided review must match the catalog hash and exact GUID + source path.
bool load(const Source &source, Catalog &out, QString &error,
          const QString &reviewPath = {});

QString categoryName(Category category);
QString availabilityName(Availability availability);
QString dlcName(Dlc dlc);

} // namespace ItemCatalog
#endif
