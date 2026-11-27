#include "itemcatalog.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace {
QJsonArray sorted(const QSet<QString> &values)
{
    QStringList list = values.values();
    list.sort(Qt::CaseSensitive);
    return QJsonArray::fromStringList(list);
}

// Fixture-only comparison, independent of the editor's active-save reader.
// Read the leading character object without confusing semicolons inside JSON strings
// with the save-section separator.
QJsonObject characterObject(const QByteArray &section)
{
    int depth = 0;
    bool quoted = false, escaped = false, started = false;
    for (qsizetype i = 0; i < section.size(); ++i) {
        const char ch = section[i];
        if (!started) {
            if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
            if (ch != '{') return {};
            started = true;
        }
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}' && --depth == 0) {
            return QJsonDocument::fromJson(section.left(i + 1)).object();
        }
    }
    return {};
}

bool compareFixture(const QString &path, const ItemCatalog::Catalog &catalog,
                    QJsonObject &report, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFile::NoError) {
        error = file.errorString();
        return false;
    }
    const auto marker = QByteArray("~SettingsSplit~");
    const auto separator = QByteArray("~NoticeMeSenpaiNoticeMe~");
    qsizetype pos = data.indexOf(marker);
    if (pos < 0) {
        error = "Missing fixture settings separator.";
        return false;
    }
    pos += marker.size();
    QSet<QString> matched, unmatched, ambiguous, arrayMismatches;
    QSet<QString> observed;
    QMap<QString, QSet<QString>> observedSlots;
    QSet<QString> slotMismatches;
    int characters = 0;
    int observations = 0;
    while (pos <= data.size()) {
        const qsizetype end = data.indexOf(separator, pos);
        const auto character = characterObject(end < 0 ? data.mid(pos) : data.mid(pos, end - pos));
        if (character.isEmpty()) {
            error = "Invalid character object in fixture.";
            return false;
        }
        ++characters;
        QMap<QString, QStringList> instanceGuids;
        for (const QString &key : {QStringLiteral("inventory"), QStringLiteral("inventoryWeapons")}) {
            if (!character.value(key).isArray()) {
                error = "Missing fixture inventory array: " + key;
                return false;
            }
            for (const auto &item : character.value(key).toArray()) {
                const QString guid = item.toObject().value("GUID").toString();
                if (guid.isEmpty()) {
                    error = "Missing definition GUID in fixture inventory.";
                    return false;
                }
                ++observations;
                observed.insert(guid);
                if (key == "inventory") {
                    const QString instance = item.toObject().value("instanceIDOnCreation").toString();
                    if (!instance.isEmpty()) instanceGuids[instance].append(guid);
                }
                const auto *entry = catalog.find(guid);
                if (!entry) {
                    unmatched.insert(guid);
                    continue;
                }
                if (entry->sourcePaths.size() != 1) {
                    ambiguous.insert(guid);
                    continue;
                }
                matched.insert(guid);
                using ItemCatalog::Category;
                if ((key == "inventoryWeapons" && entry->category == Category::Armor) ||
                    (key == "inventory" && (entry->category == Category::Weapon ||
                                             entry->category == Category::Shield)))
                    arrayMismatches.insert(guid + " (" + key + ")");
            }
        }
        // Armor slot evidence from exact instance references, not GUID suffixes.
        for (const QString &name : {QStringLiteral("Helm"), QStringLiteral("Torso"),
                                    QStringLiteral("Arms"), QStringLiteral("Legs")}) {
            const auto instances = instanceGuids.value(character.value("equiped" + name).toString());
            if (instances.size() != 1) continue;
            const QString guid = instances.first();
            const QString slot = name.toLower();
            observedSlots[guid].insert(slot);
            const auto *entry = catalog.find(guid);
            if (entry && !entry->slot.isEmpty() && entry->slot != slot)
                slotMismatches.insert(guid + " (" + slot + ")");
        }
        if (end < 0) break;
        pos = end + separator.size();
    }
    QJsonObject slotsByGuid;
    for (auto it = observedSlots.cbegin(); it != observedSlots.cend(); ++it)
        slotsByGuid.insert(it.key(), sorted(it.value()));
    report = {{"fixture", path}, {"characters", characters}, {"observations", observations},
              {"uniqueObservedGuids", observed.size()}, {"exactMatchedGuids", sorted(matched)},
              {"unmatchedGuids", sorted(unmatched)}, {"ambiguousGuids", sorted(ambiguous)},
              {"arrayCategoryMismatches", sorted(arrayMismatches)},
              {"observedArmorSlots", slotsByGuid}, {"reviewedSlotMismatches", sorted(slotMismatches)}};
    return true;
}

bool compareLegacy(const QString &path, const ItemCatalog::Catalog &catalog,
                   QJsonObject &report, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const QString source = QString::fromUtf8(file.readAll());
    if (file.error() != QFile::NoError) {
        error = file.errorString();
        return false;
    }
    const QRegularExpression block(R"(const\s+QList<QString>\s+armor\s*=\s*\{([^}]*)\};)");
    const auto match = block.match(source);
    if (!match.hasMatch()) {
        error = "Legacy armor list not found; supply the original tab_stats.cpp.";
        return false;
    }
    QSet<QString> guids, matched, missing, mismatched;
    const QRegularExpression stringLiteral(R"re("([^"]+)")re");
    auto strings = stringLiteral.globalMatch(match.captured(1));
    while (strings.hasNext()) {
        const QString guid = strings.next().captured(1);
        guids.insert(guid);
        const auto *entry = catalog.find(guid);
        if (!entry) missing.insert(guid);
        else if (entry->category != ItemCatalog::Category::Armor || entry->sourcePaths.size() != 1)
            mismatched.insert(guid);
        else matched.insert(guid);
    }
    if (guids.isEmpty()) {
        error = "Legacy armor list is empty.";
        return false;
    }
    report = {{"legacySource", path}, {"uniqueGuids", guids.size()},
              {"exactArmorMatches", sorted(matched)}, {"missingGuids", sorted(missing)},
              {"nonArmorOrAmbiguousGuids", sorted(mismatched)}};
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("tlhon-catalog");
    app.setApplicationVersion("1");
    QCommandLineParser parser;
    parser.setApplicationDescription("Read-only TLHoN item definition catalog inspector.");
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption game("game-dir", "Game installation directory.", "directory");
    const QCommandLineOption steam("steam-root", "Steam root directory.", "directory");
    const QCommandLineOption file("catalog", "Explicit Addressables catalog.json (e.g. a copy).", "file");
    const QCommandLineOption review("review", "Version-bound review manifest.", "file");
    const QCommandLineOption fixture("save-fixture", "Compare an explicitly supplied disposable save fixture (repeatable).", "file");
    const QCommandLineOption legacy("legacy-armor-source", "Compare the original tab_stats.cpp armor list.", "file");
    parser.addOptions({game, steam, file, review, fixture, legacy});
    parser.process(app);
    QTextStream err(stderr);
    const auto fail = [&](const QString &error) {
        err << error << Qt::endl;
        return 1;
    };

    if (!parser.positionalArguments().isEmpty() ||
        (parser.isSet(game) + parser.isSet(steam) + parser.isSet(file) > 1))
        return fail("Supply at most one of --game-dir, --steam-root, --catalog, and no positional arguments.");

    ItemCatalog::Source source;
    QString error;
    if (parser.isSet(file)) {
        source.catalogPath = parser.value(file);
    } else if (!ItemCatalog::locate(source, error, parser.value(game), parser.value(steam))) {
        return fail(error);
    }
    ItemCatalog::Catalog catalog;
    if (!ItemCatalog::load(source, catalog, error, parser.value(review))) return fail(error);

    QJsonObject report = catalog.report();
    QJsonArray comparisons;
    for (const auto &path : parser.values(fixture)) {
        QJsonObject comparison;
        if (!compareFixture(path, catalog, comparison, error))
            return fail(path + ": " + error);
        comparisons.append(comparison);
    }
    report.insert("saveFixtureComparisons", comparisons);
    if (parser.isSet(legacy)) {
        QJsonObject comparison;
        if (!compareLegacy(parser.value(legacy), catalog, comparison, error)) return fail(error);
        report.insert("legacyArmorComparison", comparison);
    }
    // Write only to stdout; no files (including game data or save data) modified.
    const auto json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly) || output.write(json) != json.size() || !output.flush())
        return fail("Unable to write report to stdout.");
    return 0;
}
