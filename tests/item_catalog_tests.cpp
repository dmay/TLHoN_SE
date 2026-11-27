#include "itemcatalog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstdlib>

using namespace ItemCatalog;

#define CHECK(condition) do { if (!(condition)) { \
    QTextStream(stderr) << "Failed at line " << __LINE__ << ": " << #condition << Qt::endl; \
    return EXIT_FAILURE; \
} } while (false)

bool write(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QByteArray manifest(const QByteArray &name, const QByteArray &build)
{
    return "\"AppState\" { \"appid\" \"1766100\" \"installdir\" \"" + name +
           "\" \"buildid\" \"" + build + "\" }";
}

QJsonObject reviewRow(const Entry &entry, const QString &dlc = "base", const QString &slot = {})
{
    QJsonObject row{{"guid", entry.guid}, {"sourcePath", entry.sourcePaths.first()},
                    {"status", "allow"}, {"category", categoryName(entry.category)},
                    {"evidence", "Synthetic review for tests only"}, {"dlc", dlc}};
    if (!slot.isEmpty()) row.insert("slot", slot);
    return row;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    CHECK(args.size() == 3);
    const QString repo = args[1];
    const QString cli = args[2];
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    const QString root = temporary.path();
    const QString catalogPath = root + "/catalog.json";
    const QByteArray bytes = read(repo + "/Resources/Tests/ItemCatalog/catalog.json");
    CHECK(!bytes.isEmpty());
    CHECK(write(catalogPath, bytes));

    Source source{catalogPath, "test-build"};
    Catalog catalog;
    QString error;
    CHECK(load(source, catalog, error));
    CHECK(catalog.sha256 == QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    CHECK(catalog.locatorId == "synthetic-test-only");
    CHECK(catalog.addressablesBuildHash == "synthetic-test-hash");
    CHECK(catalog.steamBuildId == "test-build");
    CHECK(catalog.internalIdCount == 22);
    CHECK(catalog.ignoredPathCount == 6);
    CHECK(catalog.repeatedPathCount == 1);
    CHECK(catalog.entries.size() == 14);
    CHECK(catalog.find("DemoArmor") && catalog.find("demoArmor"));
    CHECK(!catalog.find("DEMOARMOR"));
    CHECK(catalog.find("DemoArmor")->sourcePaths.size() == 1);
    CHECK(catalog.find("Starting_Knight_Helm")->category == Category::Armor);
    CHECK(catalog.find("Starting_Knight_Helm")->slot.isEmpty());
    CHECK(catalog.find("WelcomeArmour_TopHat")->slot.isEmpty());
    CHECK(catalog.find("AvatarBaseSword")->category == Category::Weapon);
    CHECK(catalog.find("AvatarBaseSword")->type == "Swords");
    CHECK(catalog.find("AvatarBaseShield")->category == Category::Shield);
    CHECK(catalog.find("DemoNestedShield")->category == Category::Shield);
    CHECK(catalog.find("DemoDlcArmor")->dlc == Dlc::DlcPath);
    CHECK(catalog.find("HealthPotion")->category == Category::Other);
    CHECK(catalog.find("BaseSword")->reason == "base-type-path");
    CHECK(catalog.find("BaseSword")->availability == Availability::Excluded);
    CHECK(catalog.find("ArmorIcon")->reason == "non-definition-path");
    CHECK(catalog.find("PrototypeSword")->reason == "suspected-placeholder-or-prototype");
    CHECK(catalog.find("Placeholder")->reason == "suspected-placeholder-or-prototype");
    CHECK(catalog.find("Conflict")->reason == "ambiguous-guid");
    CHECK(catalog.find("Conflict")->sourcePaths.size() == 2);
    CHECK(catalog.bulkEligible(Category::Armor, true).isEmpty());
    CHECK(catalog.bulkEligible(Category::Weapon, true).isEmpty());
    CHECK(catalog.bulkEligible(Category::Shield, true).isEmpty());
    CHECK(catalog.report().value("uniqueGuidCount").toInt() == 14);
    CHECK(catalog.report().value("reasonCounts").toObject().value("ambiguous-guid").toInt() == 1);

    const Catalog original = catalog;
    QJsonObject special = reviewRow(*catalog.find("WelcomeArmour_TopHat"), "base", "helm");
    special.insert("displayName", "Synthetic display name");
    special.insert("type", "verified synthetic armor type");
    const QJsonArray rows{
        special,
        reviewRow(*catalog.find("AvatarBaseSword")),
        reviewRow(*catalog.find("AvatarBaseShield")),
        reviewRow(*catalog.find("DemoDlcArmor"), "dlc", "torso"),
        QJsonObject{{"guid", "Placeholder"}, {"sourcePath", catalog.find("Placeholder")->sourcePaths.first()},
                    {"status", "exclude"}, {"reason", "placeholder"},
                    {"evidence", "Synthetic non-obtainable placeholder"}}
    };
    QJsonObject review{{"schemaVersion", 1}, {"catalogSha256", catalog.sha256}, {"entries", rows}};
    const QString reviewPath = root + "/review.json";
    const auto writeReview = [&](const QJsonObject &object) {
        return write(reviewPath, QJsonDocument(object).toJson());
    };
    CHECK(writeReview(review));
    CHECK(load(source, catalog, error, reviewPath));
    CHECK(catalog.bulkEligible(Category::Armor).size() == 1);
    CHECK(catalog.bulkEligible(Category::Armor, true).size() == 2);
    CHECK(catalog.bulkEligible(Category::Weapon).size() == 1);
    CHECK(catalog.bulkEligible(Category::Shield).size() == 1);
    CHECK(catalog.find("WelcomeArmour_TopHat")->displayName == "Synthetic display name");
    CHECK(catalog.find("WelcomeArmour_TopHat")->slot == "helm");
    CHECK(catalog.find("AvatarBaseSword")->displayName == "AvatarBaseSword");
    CHECK(catalog.find("Placeholder")->reason == "reviewed-placeholder");
    CHECK(catalog.find("WelcomeArmour_TopHat")->displayNameVerified);
    CHECK(catalog.find("WelcomeArmour_TopHat")->typeVerified);
    CHECK(!catalog.find("AvatarBaseSword")->displayNameVerified);
    CHECK(!catalog.find("AvatarBaseSword")->typeVerified);
    CHECK(!catalog.find("DemoDlcArmor")->bulkEligible());
    CHECK(catalog.find("DemoDlcArmor")->bulkEligible(true));

    // Invalid reviews must fail atomically rather than silently enabling a subset.
    const auto rejectRow = [&](const QJsonObject &row) {
        QJsonObject invalid = review;
        QJsonArray invalidRows = rows;
        invalidRows.append(row);
        invalid.insert("entries", invalidRows);
        return writeReview(invalid) && !load(source, catalog, error, reviewPath) &&
            catalog.entries.isEmpty() && !error.isEmpty();
    };
    CHECK(rejectRow(special)); // duplicate review
    CHECK(rejectRow(reviewRow(*original.find("Conflict"))));
    CHECK(rejectRow(reviewRow(*original.find("BaseSword"))));
    CHECK(rejectRow(reviewRow(*original.find("HealthPotion"))));
    CHECK(rejectRow(reviewRow(*original.find("DemoArmor"), "base", "hand")));
    CHECK(rejectRow(reviewRow(*original.find("DemoArmor"), "base")));
    auto row = reviewRow(*original.find("demoArmor"), "base", "arms");
    row.insert("guid", "DEMOARMOR");
    CHECK(rejectRow(row));
    row = reviewRow(*original.find("demoArmor"), "base", "arms");
    row.insert("sourcePath", "wrong-path.asset");
    CHECK(rejectRow(row));
    row = reviewRow(*original.find("demoArmor"), "base", "arms");
    row.insert("evidence", "");
    CHECK(rejectRow(row));
    auto invalid = review;
    invalid.insert("catalogSha256", "old-hash");
    CHECK(writeReview(invalid));
    CHECK(!load(source, catalog, error, reviewPath) && catalog.entries.isEmpty());
    CHECK(writeReview(review));
    CHECK(write(catalogPath, bytes + "\n"));
    CHECK(!load(source, catalog, error, reviewPath) && catalog.entries.isEmpty());
    CHECK(write(catalogPath, bytes));

    // Unknown shape, empty candidates, malformed JSON, non-string internal ID, invalid path.
    for (const QByteArray &bad : {QByteArray("{"), QByteArray("{}"),
             QByteArray("{\"m_InternalIds\":[]}"),
             QByteArray("{\"m_InternalIds\":[7]}"),
             QByteArray("{\"m_InternalIds\":[\"Assets/Design/GameplayData/Items/../bad.asset\"]}")}) {
        CHECK(write(root + "/bad.json", bad));
        CHECK(!load({root + "/bad.json", {}}, catalog, error) && catalog.entries.isEmpty());
        CHECK(!error.isEmpty());
    }
    CHECK(!load({root + "/absent.json", {}}, catalog, error));
    CHECK(!load(source, catalog, error, root + "/absent-review.json"));

    // Steam discovery (modern and legacy library layouts, explicit overrides, ambiguity).
    const QString steamRoot = root + "/Steam";
    const QString library = root + "/Second Library";
    const QString gameDir = library + "/steamapps/common/Test Game";
    const QString installed = gameDir + "/TLHON_Data/StreamingAssets/aa/catalog.json";
    const QString libraries = steamRoot + "/steamapps/libraryfolders.vdf";
    CHECK(write(installed, bytes));
    CHECK(write(library + "/steamapps/appmanifest_1766100.acf", manifest("Test Game", "12345")));
    CHECK(write(libraries, "\"libraryfolders\" { // modern\n \"1\" { \"path\" \"" +
                library.toUtf8() + "\" \"apps\" { \"1766100\" \"0\" } } }"));
    Source found;
    CHECK(locate(found, error, {}, steamRoot));
    CHECK(found.catalogPath == installed && found.steamBuildId == "12345");
    CHECK(locate(found, error, gameDir, root + "/ignored"));
    CHECK(found.catalogPath == installed && found.steamBuildId == "12345");
    CHECK(!locate(found, error, root + "/missing", steamRoot) && found.catalogPath.isEmpty());
    CHECK(write(libraries, "\"LibraryFolders\" { \"1\" \"" + library.toUtf8() + "\" }"));
    CHECK(locate(found, error, {}, steamRoot));
    CHECK(write(steamRoot + "/steamapps/common/Other Game/TLHON_Data/StreamingAssets/aa/catalog.json", bytes));
    CHECK(write(steamRoot + "/steamapps/appmanifest_1766100.acf", manifest("Other Game", "2")));
    CHECK(!locate(found, error, {}, steamRoot) && error.contains("Multiple"));
    CHECK(write(libraries, "\"libraryfolders\" {"));
    CHECK(!locate(found, error, {}, steamRoot));
    CHECK(read(installed) == bytes);

    // Verify the command-line entry point, actual repository fixtures and legacy list.
    QProcess process;
    process.start(cli, {"--catalog", catalogPath, "--save-fixture", repo + "/Resources/Tests/SaveData1.txt",
                        "--save-fixture", repo + "/Resources/Tests/SaveData.txt",
                        "--legacy-armor-source", repo + "/Resources/Tests/ItemCatalog/legacy_armor.cpp.txt"});
    CHECK(process.waitForFinished(30000));
    CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    const auto comparisons = report.value("saveFixtureComparisons").toArray();
    CHECK(comparisons.size() == 2);
    const auto first = comparisons[0].toObject();
    CHECK(first.value("characters").toInt() == 1);
    CHECK(first.value("observations").toInt() == 9);
    CHECK(first.value("exactMatchedGuids").toArray().size() == 4);
    CHECK(first.value("arrayCategoryMismatches").toArray().isEmpty());
    CHECK(first.value("observedArmorSlots").toObject().value("Starting_Knight_Helm").toArray() ==
          QJsonArray{"helm"});
    CHECK(first.value("observedArmorSlots").toObject().size() == 4);
    const auto second = comparisons[1].toObject();
    CHECK(second.value("characters").toInt() == 2);
    CHECK(second.value("observations").toInt() == 351);
    CHECK(second.value("exactMatchedGuids").toArray().size() == 5);
    CHECK(second.value("arrayCategoryMismatches").toArray().isEmpty());
    CHECK(second.value("observedArmorSlots").toObject().value("MasterChefArmor_Torso").toArray() ==
          QJsonArray{"torso"});
    CHECK(second.value("observedArmorSlots").toObject().size() == 4);
    const auto legacy = report.value("legacyArmorComparison").toObject();
    CHECK(legacy.value("uniqueGuids").toInt() == 3);
    CHECK(legacy.value("missingGuids").toArray().size() == 1);
    CHECK(legacy.value("nonArmorOrAmbiguousGuids").toArray().size() == 1);
    CHECK(legacy.value("exactArmorMatches").toArray().size() == 1);
    CHECK(read(catalogPath) == bytes);
    QTextStream(stdout) << "All item catalog checks passed." << Qt::endl;
    return EXIT_SUCCESS;
}
