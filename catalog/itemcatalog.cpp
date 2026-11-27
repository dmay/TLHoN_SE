#include "itemcatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSettings>

namespace ItemCatalog {
namespace {
const QString itemRoot = QStringLiteral("Assets/Design/GameplayData/Items/");
const QString catalogRelative = QStringLiteral("TLHON_Data/StreamingAssets/aa/catalog.json");
const QString appManifest = QStringLiteral("appmanifest_1766100.acf");

bool readFile(const QString &path, QByteArray &bytes, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    bytes = file.readAll();
    if (file.error() != QFile::NoError) {
        error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool readJson(const QByteArray &bytes, QJsonObject &out, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Expected a JSON object: %1").arg(parseError.errorString());
        return false;
    }
    out = document.object();
    return true;
}

// Minimal Valve KeyValues reader for Steam's quoted libraryfolders.vdf and
// appmanifest files. Interpret quoted escapes without dropping literal slashes.
struct Vdf {
    QString value;
    QMap<QString, Vdf> children;
};

class VdfReader {
public:
    explicit VdfReader(const QByteArray &bytes) : text(QString::fromUtf8(bytes)) {}
    bool read(Vdf &out) { return object(out, false, 0); }

private:
    QString text;
    qsizetype pos = 0;

    void skip()
    {
        while (pos < text.size()) {
            if (text[pos].isSpace() || text[pos] == QChar(0xfeff)) {
                ++pos;
            } else if (text.mid(pos, 2) == QStringLiteral("//")) {
                while (pos < text.size() && text[pos] != '\n') ++pos;
            } else {
                break;
            }
        }
    }
    bool quoted(QString &result)
    {
        skip();
        if (pos >= text.size() || text[pos++] != '"') return false;
        while (pos < text.size()) {
            const QChar ch = text[pos++];
            if (ch == '"') return true;
            if (ch == '\\' && pos < text.size() &&
                (text[pos] == '\\' || text[pos] == '"')) {
                result += text[pos++];
            } else {
                result += ch;
            }
        }
        return false;
    }
    bool object(Vdf &out, bool nested, int depth)
    {
        if (depth > 32) return false;
        while (true) {
            skip();
            if (pos == text.size()) return !nested;
            if (text[pos] == '}') {
                ++pos;
                return nested;
            }
            QString key;
            if (!quoted(key) || out.children.contains(key)) return false;
            skip();
            Vdf child;
            if (pos < text.size() && text[pos] == '{') {
                ++pos;
                if (!object(child, true, depth + 1)) return false;
            } else if (!quoted(child.value)) {
                return false;
            }
            out.children.insert(key, child);
        }
    }
};

bool readVdf(const QString &path, Vdf &out, QString &error)
{
    QByteArray bytes;
    if (!readFile(path, bytes, error)) return false;
    if (!VdfReader(bytes).read(out)) {
        error = QStringLiteral("Unrecognized Steam metadata: %1").arg(path);
        return false;
    }
    return true;
}

bool safeComponent(const QString &s)
{
    return !s.isEmpty() && s != "." && s != ".." &&
           !s.contains('/') && !s.contains('\\') && !s.contains(':') &&
           !s.contains(QChar(0));
}

bool manifestSource(const QString &library, Source &out, QString &error)
{
    const QDir steamapps(QDir(library).filePath("steamapps"));
    const QString manifestPath = steamapps.filePath(appManifest);
    if (!QFileInfo::exists(manifestPath)) return true;
    Vdf manifest;
    if (!readVdf(manifestPath, manifest, error)) return false;
    const auto state = manifest.children.value("AppState").children;
    const QString install = state.value("installdir").value;
    if (state.value("appid").value != "1766100" || !safeComponent(install)) {
        error = QStringLiteral("Invalid TLHoN Steam manifest: %1").arg(manifestPath);
        return false;
    }
    const QString path = steamapps.filePath("common/" + install + "/" + catalogRelative);
    if (QFileInfo(path).isFile()) {
        out.catalogPath = QFileInfo(path).absoluteFilePath();
        out.steamBuildId = state.value("buildid").value;
    }
    return true;
}

QStringList defaultSteamRoots()
{
    QStringList roots;
#ifdef Q_OS_WIN
    const QSettings steam("HKEY_CURRENT_USER\\Software\\Valve\\Steam", QSettings::NativeFormat);
    roots << steam.value("SteamPath").toString();
    for (const auto &name : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const QString dir = qEnvironmentVariable(name);
        if (!dir.isEmpty()) roots << QDir(dir).filePath("Steam");
    }
#else
    roots << QDir::home().filePath(".steam/steam")
          << QDir::home().filePath(".local/share/Steam");
#endif
    roots.removeAll(QString());
    return roots;
}

bool isOneOf(const QString &value, const QStringList &choices)
{
    return choices.contains(value, Qt::CaseSensitive);
}

void classify(Entry &entry, const QStringList &folders)
{
    const QString top = folders.value(0);
    if (isOneOf(top, {"Armour", "DLCArmours", "StartingArmors"})) {
        entry.category = Category::Armor;
        entry.type = "armor";
    } else if (top == "Shields" ||
               (top == "Weapons" && folders.contains("Shields"))) {
        entry.category = Category::Shield;
        entry.type = "shield";
    } else if (top == "Weapons") {
        entry.category = Category::Weapon;
        entry.type = folders.size() > 1 ? folders.last() : QStringLiteral("weapon");
    } else {
        entry.type = top;
    }
    entry.reason = "unreviewed-definition";

    if (top == "DLCArmours" || folders.contains("DLC") || folders.contains("DLCWeapons")) {
        entry.dlc = Dlc::DlcPath;
        entry.reason = "dlc-entitlement-unverified";
    }
    // These are conservative review signals, not a way to infer a grantable
    // item from a name. A verified slot/name must come from explicit review.
    for (const QString &folder : folders) {
        if (isOneOf(folder, {"BaseTypes", "Textures", "Materials", "Prefabs", "Icons"})) {
            entry.availability = Availability::Excluded;
            entry.reason = folder == "BaseTypes" ? "base-type-path" : "non-definition-path";
            return;
        }
    }
    const QString hint = (folders.join('/') + '/' + entry.guid).toLower();
    if (hint.contains("placeholder") || hint.contains("prototype")) {
        entry.reason = "suspected-placeholder-or-prototype";
    }
}

bool applyReview(const QJsonObject &review, Catalog &catalog, QString &error)
{
    if (review.value("schemaVersion") != QJsonValue(1) ||
        review.value("catalogSha256").toString() != catalog.sha256 ||
        !review.value("entries").isArray()) {
        error = "Review schema or catalog SHA-256 mismatch; regenerate/review for this version.";
        return false;
    }

    QSet<QString> reviewed;
    for (const auto &value : review.value("entries").toArray()) {
        const auto row = value.toObject();
        const QString guid = row.value("guid").toString();
        const QString path = row.value("sourcePath").toString();
        const QString status = row.value("status").toString();
        const QString evidence = row.value("evidence").toString();
        const auto invalid = [&]() {
            error = QStringLiteral("Invalid or conflicting review entry for GUID '%1'.").arg(guid);
            return false;
        };
        auto it = catalog.entries.find(guid);
        if (!value.isObject() || guid.isEmpty() || reviewed.contains(guid) ||
            it == catalog.entries.end() || !it->sourcePaths.contains(path) ||
            evidence.trimmed().isEmpty()) {
            return invalid();
        }
        reviewed.insert(guid);
        if (status == "exclude") {
            const QString reason = row.value("reason").toString();
            if (!isOneOf(reason, {"non-obtainable", "placeholder", "prototype", "dlc-unverified", "other"}))
                return invalid();
            // Preserve structural exclusion diagnostics such as ambiguous GUIDs.
            if (it->availability != Availability::Excluded) {
                it->availability = Availability::Excluded;
                it->reason = "reviewed-" + reason;
            }
            it->evidence = evidence;
            continue;
        }
        if (status != "allow" || it->availability == Availability::Excluded ||
            it->category == Category::Other ||
            row.value("category").toString() != categoryName(it->category)) {
            return invalid();
        }

        const QString dlc = row.value("dlc").toString();
        if (dlc == "dlc") {
            it->dlc = Dlc::RequiresDlc;
        } else if (dlc == "base" && it->dlc != Dlc::DlcPath) {
            it->dlc = Dlc::Base;
        } else {
            return invalid();
        }

        const QString slot = row.value("slot").toString();
        if (it->category == Category::Armor) {
            if (!isOneOf(slot, {"helm", "torso", "arms", "legs"})) return invalid();
        } else if (row.contains("slot")) {
            return invalid();
        }
        if ((row.contains("displayName") && !row.value("displayName").isString()) ||
            (row.contains("type") && !row.value("type").isString())) return invalid();
        it->slot = slot;
        if (!row.value("displayName").toString().trimmed().isEmpty()) {
            it->displayName = row.value("displayName").toString();
            it->displayNameVerified = true;
        }
        if (!row.value("type").toString().trimmed().isEmpty()) {
            it->type = row.value("type").toString();
            it->typeVerified = true;
        }
        it->availability = Availability::Validated;
        it->reason = "reviewed-allowlist";
        it->evidence = evidence;
    }
    return true;
}
} // namespace

QString categoryName(Category category)
{
    switch (category) {
    case Category::Armor: return "armor";
    case Category::Weapon: return "weapon";
    case Category::Shield: return "shield";
    case Category::Other: return "other";
    }
    return {};
}

QString availabilityName(Availability availability)
{
    switch (availability) {
    case Availability::Unresolved: return "unresolved";
    case Availability::Excluded: return "excluded";
    case Availability::Validated: return "validated";
    }
    return {};
}

QString dlcName(Dlc dlc)
{
    switch (dlc) {
    case Dlc::Unknown: return "unknown";
    case Dlc::DlcPath: return "dlc-path";
    case Dlc::Base: return "base";
    case Dlc::RequiresDlc: return "dlc";
    }
    return {};
}

bool Entry::bulkEligible(bool dlcEntitled) const
{
    return availability == Availability::Validated && sourcePaths.size() == 1 &&
           category != Category::Other && !evidence.trimmed().isEmpty() &&
           (category != Category::Armor || isOneOf(slot, {"helm", "torso", "arms", "legs"})) &&
           (dlc == Dlc::Base || (dlcEntitled && dlc == Dlc::RequiresDlc));
}

const Entry *Catalog::find(const QString &guid) const
{
    const auto it = entries.constFind(guid);
    return it == entries.constEnd() ? nullptr : &it.value();
}

QList<Entry> Catalog::bulkEligible(Category category, bool dlcEntitled) const
{
    QList<Entry> result;
    for (const auto &entry : entries) {
        if (entry.category == category && entry.bulkEligible(dlcEntitled)) result.append(entry);
    }
    return result;
}

QJsonObject Catalog::report() const
{
    QJsonObject categories, reasons, statuses;
    QJsonArray rows;
    const auto count = [](QJsonObject &counts, const QString &key) {
        counts.insert(key, counts.value(key).toInt() + 1);
    };
    for (const auto &entry : entries) {
        const QString category = categoryName(entry.category);
        const QString status = availabilityName(entry.availability);
        count(categories, category);
        count(reasons, entry.reason);
        count(statuses, status);
        rows.append(QJsonObject{
            {"guid", entry.guid}, {"sourcePaths", QJsonArray::fromStringList(entry.sourcePaths)},
            {"category", category}, {"type", entry.type}, {"typeVerified", entry.typeVerified}, {"slot", entry.slot},
            {"displayName", entry.displayName}, {"displayNameVerified", entry.displayNameVerified},
            {"dlc", dlcName(entry.dlc)},
            {"availability", status}, {"reason", entry.reason}, {"evidence", entry.evidence},
            {"bulkEligibleWithoutDlc", entry.bulkEligible()}
        });
    }
    return {{"schemaVersion", 1}, {"steamAppId", "1766100"}, {"catalogSha256", sha256},
            {"steamBuildId", steamBuildId}, {"locatorId", locatorId},
            {"addressablesBuildHash", addressablesBuildHash}, {"internalIdCount", internalIdCount},
            {"ignoredPathCount", ignoredPathCount}, {"repeatedPathCount", repeatedPathCount},
            {"uniqueGuidCount", entries.size()}, {"categoryCounts", categories},
            {"reasonCounts", reasons}, {"availabilityCounts", statuses}, {"entries", rows}};
}

bool locate(Source &out, QString &error, const QString &gameDir, const QString &steamRoot)
{
    out = {};
    error.clear();
    if (!gameDir.isEmpty()) {
        const QFileInfo path(QDir(gameDir).filePath(catalogRelative));
        if (!path.isFile()) {
            error = QStringLiteral("No TLHoN catalog at %1").arg(path.absoluteFilePath());
            return false;
        }
        out.catalogPath = path.absoluteFilePath();
        // A conventional Steam game folder can supply its build ID, but the
        // catalog hash alone is sufficient for non-Steam and copied installs.
        QDir common(gameDir);
        if (common.cdUp() && common.dirName() == "common" && common.cdUp() &&
            common.dirName() == "steamapps" && common.cdUp()) {
            Source autoSource;
            QString ignored;
            if (manifestSource(common.absolutePath(), autoSource, ignored) &&
                QFileInfo(autoSource.catalogPath).canonicalFilePath() == path.canonicalFilePath()) {
                out.steamBuildId = autoSource.steamBuildId;
            }
        }
        return true;
    }

    const QStringList roots = steamRoot.isEmpty() ? defaultSteamRoots() : QStringList{steamRoot};
    QSet<QString> libraries;
    for (const auto &root : roots) {
        if (!QFileInfo(root).isDir()) continue;
        libraries.insert(QFileInfo(root).canonicalFilePath());
        const QString libraryFile = QDir(root).filePath("steamapps/libraryfolders.vdf");
        if (!QFileInfo::exists(libraryFile)) continue;
        Vdf vdf;
        if (!readVdf(libraryFile, vdf, error)) return false;
        const auto key = vdf.children.contains("libraryfolders")
            ? QStringLiteral("libraryfolders") : QStringLiteral("LibraryFolders");
        if (!vdf.children.contains(key)) {
            error = QStringLiteral("Unrecognized Steam library list: %1").arg(libraryFile);
            return false;
        }
        const auto folders = vdf.children.value(key).children;
        for (auto it = folders.begin(); it != folders.end(); ++it) {
            bool numeric = false;
            it.key().toUInt(&numeric);
            if (!numeric) continue;
            const QString library = it->children.value("path").value.isEmpty()
                ? it->value : it->children.value("path").value;
            if (QDir::isAbsolutePath(library) && QFileInfo(library).isDir())
                libraries.insert(QFileInfo(library).canonicalFilePath());
        }
    }
    QMap<QString, Source> matches;
    for (const auto &library : libraries) {
        Source found;
        if (!manifestSource(library, found, error)) return false;
        if (!found.catalogPath.isEmpty())
            matches.insert(QFileInfo(found.catalogPath).canonicalFilePath(), found);
    }
    if (matches.size() != 1) {
        error = matches.isEmpty() ? "TLHoN Steam catalog not found; supply a game directory."
                                  : "Multiple TLHoN installations found; supply a game directory.";
        return false;
    }
    out = matches.first();
    return true;
}

bool load(const Source &source, Catalog &out, QString &error, const QString &reviewPath)
{
    out = {};
    error.clear();
    Catalog catalog;
    QByteArray bytes;
    QJsonObject document;
    if (!readFile(source.catalogPath, bytes, error) || !readJson(bytes, document, error)) return false;
    if (!document.value("m_InternalIds").isArray()) {
        error = "Unrecognized Addressables catalog: m_InternalIds must be an array.";
        return false;
    }
    catalog.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    catalog.steamBuildId = source.steamBuildId;
    catalog.locatorId = document.value("m_LocatorId").toString();
    catalog.addressablesBuildHash = document.value("m_BuildResultHash").toString();

    const auto ids = document.value("m_InternalIds").toArray();
    catalog.internalIdCount = ids.size();
    for (const auto &id : ids) {
        if (!id.isString() || id.toString().isEmpty()) {
            error = "Unrecognized Addressables catalog: invalid m_InternalIds entry.";
            return false;
        }
        const QString path = id.toString();
        if (!path.startsWith(itemRoot) || !path.endsWith(".asset")) {
            ++catalog.ignoredPathCount;
            continue;
        }
        QStringList parts = path.mid(itemRoot.size()).split('/');
        const QString file = parts.takeLast();
        const QString guid = file.chopped(6);
        bool valid = safeComponent(guid);
        for (const auto &part : parts) valid = valid && safeComponent(part);
        if (!valid) {
            error = QStringLiteral("Invalid item definition path: %1").arg(path);
            return false;
        }
        auto it = catalog.entries.find(guid);
        if (it == catalog.entries.end()) {
            Entry entry;
            entry.guid = guid;
            entry.displayName = guid;
            entry.sourcePaths.append(path);
            classify(entry, parts);
            catalog.entries.insert(guid, entry);
        } else if (it->sourcePaths.contains(path)) {
            ++catalog.repeatedPathCount;
        } else {
            it->sourcePaths.append(path);
            it->sourcePaths.sort(Qt::CaseSensitive);
            // Do not implicitly choose the first definition in a collision.
            it->category = Category::Other;
            it->type.clear();
            it->dlc = Dlc::Unknown;
            it->availability = Availability::Excluded;
            it->reason = "ambiguous-guid";
        }
    }
    if (catalog.entries.isEmpty()) {
        error = "No recognized TLHoN item .asset paths; catalog format or root may have changed.";
        return false;
    }
    if (!reviewPath.isEmpty()) {
        QJsonObject review;
        if (!readFile(reviewPath, bytes, error) || !readJson(bytes, review, error) ||
            !applyReview(review, catalog, error)) return false;
    }
    out = catalog;
    return true;
}
} // namespace ItemCatalog
