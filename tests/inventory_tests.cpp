#include "inventorymodel.h"
#include "inventoryproperties.h"
#include "jsonparser.h"
#include "savestore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace Inventory;
using ItemCatalog::Category;

namespace {
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
QByteArray fixture(const QString &name)
{
    return readFile(QDir(FIXTURE_DIR).filePath(name));
}

bool testCatalog(const QString &dir, ItemCatalog::Catalog &catalog, QString &error,
                 bool reviewed = true)
{
    // Invented paths & review, NEVER a real game's allow-list.
    const QJsonArray ids {
        "Assets/Design/GameplayData/Items/Armour/TestArmor.asset",
        "Assets/Design/GameplayData/Items/Armour/testArmor.asset",
        "Assets/Design/GameplayData/Items/Armour/DLCArmours/TestDlcArmor.asset",
        "Assets/Design/GameplayData/Items/Weapons/Swords/TestSword.asset",
        "Assets/Design/GameplayData/Items/Shields/TestShield.asset",
        "Assets/Design/GameplayData/Items/Weapons/Swords/UnreviewedSword.asset",
        "Assets/Design/GameplayData/Items/Weapons/BaseTypes/BaseSword.asset",
        "Assets/Design/GameplayData/Items/Consumables/TestPotion.asset"
    };
    const auto path = dir + "/catalog.json";
    if (!put(path, QJsonDocument(QJsonObject{{"m_InternalIds", ids}}).toJson())) return false;
    const ItemCatalog::Source source{path, "synthetic-build"};
    if (!ItemCatalog::load(source, catalog, error)) return false;
    if (!reviewed) return true;

    QJsonArray entries;
    for (const auto &guid : {"TestArmor", "testArmor", "TestDlcArmor", "TestSword", "TestShield"}) {
        const auto *entry = catalog.find(QLatin1String(guid));
        if (!entry) return false;
        QJsonObject row{{"guid", entry->guid}, {"sourcePath", entry->sourcePaths.first()},
            {"status", "allow"}, {"category", ItemCatalog::categoryName(entry->category)},
            {"dlc", entry->dlc == ItemCatalog::Dlc::DlcPath ? "dlc" : "base"},
            {"evidence", "Invented review for automated tests only"}};
        if (entry->category == Category::Armor) row.insert("slot", "helm");
        entries.append(row);
    }
    const auto review = dir + "/review.json";
    if (!put(review, QJsonDocument(QJsonObject{{"schemaVersion", 1},
            {"catalogSha256", catalog.sha256}, {"entries", entries}}).toJson())) return false;
    return ItemCatalog::load(source, catalog, error, review);
}

QJsonObject withoutIdentity(QJsonObject object)
{
    object.remove("GUID");
    object.remove("instanceIDOnCreation");
    return object;
}

QJsonObject objByGuid(const QJsonObject &character, ArrayKind kind, const QString &guid)
{
    for (const auto &item : character.value(arrayKey(kind)).toArray()) {
        if (item.toObject().value("GUID").toString() == guid) return item.toObject();
    }
    return {};
}
}

class InventoryTests : public QObject
{
    Q_OBJECT
private slots:
    void fixtureIdentitiesAndSchemas()
    {
        QString error;
        for (const auto *file : {"SaveData.txt", "SaveData1.txt"}) {
            SaveDocument doc;
            const auto bytes = fixture(QLatin1String(file));
            QVERIFY2(SaveDocument::parse(bytes, doc, error), qPrintable(error));
            for (int index = 0; index < doc.characters().size(); ++index) {
                QList<Instance> instances;
                QVERIFY2(Inventory::read(doc, index, instances, error), qPrintable(error));
                const auto &slot = doc.characters().at(index);
                QCOMPARE(instances.size(), slot.character.value("inventory").toArray().size() +
                                           slot.character.value("inventoryWeapons").toArray().size());
                for (const auto &instance : instances) {
                    const auto *match = Inventory::find(instances, instance.kind, instance.instanceId);
                    QVERIFY(match);
                    QCOMPARE(match->fields, instance.fields);
                    QCOMPARE(match->guid, instance.guid);
                }
            }
            QList<Instance> output;
            QVERIFY(!Inventory::read(doc, -1, output, error));
            QVERIFY(!Inventory::read(doc, doc.characters().size(), output, error));
            QVERIFY(output.isEmpty());
            QCOMPARE(doc.serialize(), bytes);
        }
    }

    void addThroughProtectedStore()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const SavePaths paths(temp.path());
        QString error;
        ItemCatalog::Catalog catalog;
        QVERIFY2(testCatalog(temp.path(), catalog, error), qPrintable(error));

        // Use slot 1 of a disposable real-format two-character fixture.
        SaveDocument original;
        QVERIFY2(SaveDocument::parse(fixture("SaveData.txt"), original, error), qPrintable(error));
        auto &slot = original.editCharacter(1);
        auto generic = slot.character.value("inventory").toArray();
        auto duplicate = generic.first().toObject();
        duplicate.insert("instanceIDOnCreation", "distinct-duplicate");
        duplicate.insert("futureProperty", QJsonObject{{"nested", QJsonArray{1, true, "keep"}}});
        generic.append(duplicate);
        // A definition in the wrong array must not suppress a proper grant,
        // and must not be removed/migrated by the editor.
        auto unknown = duplicate;
        unknown.insert("GUID", "TestSword");
        unknown.insert("instanceIDOnCreation", "unknown-to-this-array");
        generic.append(unknown);
        slot.character.insert("inventory", generic);
        slot.character.insert("futureCharacterField", QJsonObject{{"keep", 42}});
        const auto before = original.serialize();
        QVERIFY(put(paths.primary(), before));
        QVERIFY(put(paths.gameBackup(), "companion"));
        QVERIFY(put(paths.legacyEditorBackup(), "legacy"));

        SaveStore store(paths);
        QVERIFY2(store.load(error), qPrintable(error));
        const auto initial = store.document();
        const auto originalGeneric = initial.characters().at(1).character.value("inventory").toArray();
        const auto originalWeapons = initial.characters().at(1).character.value("inventoryWeapons").toArray();
        qsizetype added = -1;
        QVERIFY2(store.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addOne(candidate, catalog, "TestSword", added, e);
        }, error), qPrintable(error));
        QCOMPARE(added, 1);
        auto current = store.document().characters().at(1).character;
        QCOMPARE(current.value("inventory").toArray(), originalGeneric);
        QCOMPARE(current.value("inventoryWeapons").toArray().size(), originalWeapons.size() + 1);

        QVERIFY2(store.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addAllMissing(candidate, catalog, {Category::Armor, Category::Weapon, Category::Shield},
                                 added, e);
        }, error), qPrintable(error));
        QCOMPARE(added, 3); // exact-case distinct armor x2 + shield; sword exists, DLC denied
        current = store.document().characters().at(1).character;
        const auto afterGeneric = current.value("inventory").toArray();
        const auto afterWeapons = current.value("inventoryWeapons").toArray();
        QCOMPARE(afterGeneric.size(), originalGeneric.size() + 2);
        QCOMPARE(afterWeapons.size(), originalWeapons.size() + 2);
        for (qsizetype i = 0; i < originalGeneric.size(); ++i)
            QCOMPARE(afterGeneric.at(i), originalGeneric.at(i));
        for (qsizetype i = 0; i < originalWeapons.size(); ++i)
            QCOMPARE(afterWeapons.at(i), originalWeapons.at(i));

        // Compare the full generated record (excluding definition/instance
        // identity) against observed baseline examples, not loose key tests.
        SaveDocument small;
        QVERIFY2(SaveDocument::parse(fixture("SaveData1.txt"), small, error), qPrintable(error));
        const auto &reference = small.characters().first().character;
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Generic, "TestArmor")),
                 withoutIdentity(objByGuid(reference, ArrayKind::Generic, "Starting_Knight_Helm")));
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Weapon, "TestSword")),
                 withoutIdentity(objByGuid(reference, ArrayKind::Weapon, "AvatarBaseSword")));
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Weapon, "TestShield")),
                 withoutIdentity(objByGuid(reference, ArrayKind::Weapon, "AvatarBaseShield")));

        const auto &largeReference = initial.characters().first().character;
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Generic, "TestArmor")),
                 withoutIdentity(objByGuid(largeReference, ArrayKind::Generic, "SkeletonArmor_Helm")));
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Weapon, "TestSword")),
                 withoutIdentity(objByGuid(largeReference, ArrayKind::Weapon, "DaggerofCycles_Dagger")));
        QCOMPARE(withoutIdentity(objByGuid(current, ArrayKind::Weapon, "TestShield")),
                 withoutIdentity(objByGuid(largeReference, ArrayKind::Weapon, "Degraded_SmallShield")));

        QList<Instance> instances;
        QVERIFY2(Inventory::read(store.document(), 1, instances, error), qPrintable(error));
        QSet<QString> ids;
        for (const auto &item : instances) {
            QVERIFY(!ids.contains(item.instanceId));
            ids.insert(item.instanceId);
            if (item.guid == "TestArmor" || item.guid == "testArmor" ||
                (item.guid == "TestSword" && item.kind == ArrayKind::Weapon) || item.guid == "TestShield") {
                const QUuid uuid(item.instanceId);
                QVERIFY(!uuid.isNull());
                QCOMPARE(item.instanceId, uuid.toString(QUuid::WithoutBraces));
            }
        }

        // Reproduce exactly the intended two-field edit of the original
        // envelope; all unedited JSON fields, other slot/sections and companion
        // remain intact, including opaque bytes between objects.
        auto expected = initial;
        expected.editCharacter(1).character.insert("inventory", afterGeneric);
        expected.editCharacter(1).character.insert("inventoryWeapons", afterWeapons);
        const auto persisted = readFile(paths.primary());
        QCOMPARE(persisted, expected.serialize());
        QCOMPARE(readFile(paths.gameBackup()), QByteArray("companion"));
        QCOMPARE(readFile(paths.legacyEditorBackup()), QByteArray("legacy"));
        SaveStore reloaded(paths);
        QVERIFY2(reloaded.load(error), qPrintable(error));
        QCOMPARE(reloaded.document().characters().at(1).character, current);

        QFile file(paths.primary());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(1577836800, Qt::UTC), QFileDevice::FileModificationTime));
        file.close();
        const auto mtime = QFileInfo(paths.primary()).lastModified();
        QVERIFY2(reloaded.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addAllMissing(candidate, catalog, {Category::Armor, Category::Weapon, Category::Shield},
                                 added, e);
        }, error), qPrintable(error));
        QCOMPARE(added, 0);
        QCOMPARE(readFile(paths.primary()), persisted);
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), mtime);

        QVERIFY(!reloaded.mutateCharacterChecked(-1, [](SaveCharacter &, QString &) { return true; }, error));
        QVERIFY(!reloaded.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addMissing(candidate, catalog, {"TestDlcArmor", "Unknown"}, added, e, true);
        }, error));
        QCOMPARE(added, 0);
        QCOMPARE(readFile(paths.primary()), persisted);
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), mtime);
        // A callback can fail even after altering its detached candidate.
        QVERIFY(!reloaded.mutateCharacterChecked(1, [](SaveCharacter &candidate, QString &e) {
            candidate.character.insert("memory", -1);
            e = "intentional rejection";
            return false;
        }, error));
        QCOMPARE(error, QString("intentional rejection"));
        QCOMPARE(reloaded.document().characters().at(1).character, current);
        QCOMPARE(readFile(paths.primary()), persisted);

        QVERIFY2(reloaded.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addOne(candidate, catalog, "TestDlcArmor", added, e, true);
        }, error), qPrintable(error));
        QCOMPARE(added, 1);
        QVERIFY(!objByGuid(reloaded.document().characters().at(1).character, ArrayKind::Generic,
                           "TestDlcArmor").isEmpty());
        const auto outside = reloaded.document().serialize() + " ";
        QVERIFY(put(paths.primary(), outside));
        QVERIFY(!reloaded.mutateCharacterChecked(1, [&](SaveCharacter &candidate, QString &e) {
            return addOne(candidate, catalog, "TestDlcArmor", added, e, true);
        }, error));
        QCOMPARE(readFile(paths.primary()), outside);
        QCOMPARE(readFile(paths.gameBackup()), QByteArray("companion"));
    }

    void propertyMutationsThroughAppFacade()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        SavePaths paths(temp.path());
        QString error;
        SaveDocument original;
        QVERIFY2(SaveDocument::parse(fixture("SaveData.txt"), original, error), qPrintable(error));
        auto &slot = original.editCharacter(1);
        auto generic = slot.character.value("inventory").toArray();
        qsizetype genericIndex = -1;
        for (qsizetype i = 0; i < generic.size(); ++i)
            if (generic.at(i).toObject().value("GUID").toString() == "Consumable_SlowStaminaRestore")
                genericIndex = i;
        QVERIFY(genericIndex >= 0);
        auto record = generic.at(genericIndex).toObject();
        QCOMPARE(record.value("uses").toInt(), 1);
        QCOMPARE(record.value("hasSeen").toBool(), false);
        const auto id = record.value("instanceIDOnCreation").toString();
        record.insert("futureProperty", QJsonObject{{"nested", QJsonArray{2, "keep"}}});
        generic.replace(genericIndex, record);
        auto duplicate = record;
        duplicate.insert("instanceIDOnCreation", "test-distinct-instance");
        generic.append(duplicate);
        slot.character.insert("inventory", generic);
        auto weapons = slot.character.value("inventoryWeapons").toArray();
        qsizetype weaponIndex = -1;
        for (qsizetype i = 0; i < weapons.size(); ++i)
            if (weapons.at(i).toObject().value("GUID").toString() == "DatumDiggingDagger")
                weaponIndex = i;
        QVERIFY(weaponIndex >= 0);
        const auto weapon = weapons.at(weaponIndex).toObject();
        const auto weaponId = weapon.value("instanceIDOnCreation").toString();
        // Disposable test: hold references to both edited instance IDs.
        slot.character.insert("slottedRightHands", QJsonArray{weaponId, "Base_Punch"});
        slot.character.insert("equipedConsumables", QJsonArray{id, "NULL", "NULL", "NULL", "NULL"});
        QVERIFY(put(paths.primary(), original.serialize()));
        QVERIFY(put(paths.gameBackup(), "game backup"));
        QVERIFY(put(paths.legacyEditorBackup(), "legacy backup"));

        JsonParser::configure(paths);
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        PropertySelection selected;
        QVERIFY2(selectProperties(JsonParser::characters(), 1, ArrayKind::Generic, id,
                                  selected, error), qPrintable(error));
        const auto options = propertyOptions(JsonParser::characters().at(1), selected.item);
        QVERIFY(options.readOnlyReason.isEmpty());
        QVERIFY(options.usesReadOnlyReason.isEmpty());
        QVERIFY(options.hasSeenReadOnlyReason.isEmpty());
        QVERIFY(!options.flagsReadOnlyReason.isEmpty());
        QVERIFY(!options.ngPlusReadOnlyReason.isEmpty());

        const auto before = readFile(paths.primary());
        PropertyEdits edit;
        for (int badCount : {-2, -1, 0, 3, 100000}) {
            edit.uses = badCount;
            QVERIFY(!applyProperties(1, selected, edit, error));
            QCOMPARE(readFile(paths.primary()), before);
        }
        edit.uses = 2; // missing partner seen change
        QVERIFY(!applyProperties(1, selected, edit, error));
        QCOMPARE(readFile(paths.primary()), before);
        edit.hasSeen = true;
        QVERIFY2(applyProperties(1, selected, edit, error), qPrintable(error));
        auto expected = original;
        auto expectedGeneric = generic;
        auto expectedRecord = record;
        expectedRecord.insert("uses", 2);
        expectedRecord.insert("hasSeen", true);
        expectedGeneric.replace(genericIndex, expectedRecord);
        expected.editCharacter(1).character.insert("inventory", expectedGeneric);
        QCOMPARE(readFile(paths.primary()), expected.serialize());
        // Original record snapshot is stale, even when the ID still exists.
        QVERIFY(!applyProperties(1, selected, edit, error));
        QVERIFY(!applyProperties(0, selected, edit, error));
        QCOMPARE(readFile(paths.primary()), expected.serialize());
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QVERIFY2(selectProperties(JsonParser::characters(), 1, ArrayKind::Generic,
                                  "test-distinct-instance", selected, error), qPrintable(error));
        QCOMPARE(selected.item.fields, duplicate);
        QVERIFY2(applyProperties(1, selected, edit, error), qPrintable(error));
        duplicate.insert("uses", 2);
        duplicate.insert("hasSeen", true);
        expectedGeneric.replace(expectedGeneric.size() - 1, duplicate);
        expected.editCharacter(1).character.insert("inventory", expectedGeneric);
        QCOMPARE(readFile(paths.primary()), expected.serialize());

        // The real infusion enum and eligibility cannot be inferred from
        // fixtures. An INVENTED, test-only review exercises the full typed
        // weapon path without enabling unsupported game edits.
        const QList<PropertyRule> testRules {
            {ArrayKind::Weapon, "DatumDiggingDagger", {},
             {{5, 0, false}, {4, 1, false}, {0, 0, false}},
             "Synthetic test-only complete state review; not game evidence"}
        };
        QVERIFY2(selectProperties(JsonParser::characters(), 1, ArrayKind::Weapon,
                                  weaponId, selected, error), qPrintable(error));
        QVERIFY(!propertyOptions(JsonParser::characters().at(1), selected.item).readOnlyReason.isEmpty());
        edit = {};
        edit.level = 4;
        edit.infusionType = 1;
        QVERIFY(!applyProperties(1, selected, edit, error)); // default rules fail closed
        QCOMPARE(readFile(paths.primary()), expected.serialize());
        QVERIFY2(applyProperties(1, selected, edit, error, testRules), qPrintable(error));
        auto expectedWeapon = weapon;
        expectedWeapon.insert("level", 4);
        expectedWeapon.insert("infusionType", 1);
        weapons.replace(weaponIndex, expectedWeapon);
        expected.editCharacter(1).character.insert("inventoryWeapons", weapons);
        QCOMPARE(readFile(paths.primary()), expected.serialize());
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QVERIFY2(selectProperties(JsonParser::characters(), 1, ArrayKind::Weapon,
                                  weaponId, selected, error), qPrintable(error));
        QCOMPARE(selected.item.fields, expectedWeapon);
        const auto weaponOptions = propertyOptions(JsonParser::characters().at(1), selected.item, testRules);
        QVERIFY(weaponOptions.levelReadOnlyReason.isEmpty());
        QVERIFY(weaponOptions.infusionReadOnlyReason.isEmpty());
        QVERIFY(!weaponOptions.hasSeenReadOnlyReason.isEmpty());

        // Exact no-op must preserve even file timestamps.
        QFile file(paths.primary());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(1577836800, Qt::UTC),
                                 QFileDevice::FileModificationTime));
        file.close();
        const auto mtime = QFileInfo(paths.primary()).lastModified();
        QVERIFY2(applyProperties(1, selected, edit, error, testRules), qPrintable(error));
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), mtime);
        const auto stable = expected.serialize();
        QList<PropertyEdits> invalid;
        PropertyEdits bad;
        bad.level = -1; invalid.append(bad);
        bad.level = 10000; invalid.append(bad);
        bad = {}; bad.infusionType = -1; invalid.append(bad);
        bad.infusionType = 9999; invalid.append(bad);
        bad.infusionType = 0; invalid.append(bad); // 4/0 was never approved
        bad = {}; bad.uses = 3; invalid.append(bad); // wrong array
        for (const auto &invalidEdit : invalid) {
            QVERIFY(!applyProperties(1, selected, invalidEdit, error, testRules));
            QVERIFY(!error.isEmpty());
            QCOMPARE(readFile(paths.primary()), stable);
            QCOMPARE(QFileInfo(paths.primary()).lastModified(), mtime);
        }
        edit.level = 0;
        edit.infusionType = 0;
        QVERIFY2(applyProperties(1, selected, edit, error, testRules), qPrintable(error));
        expectedWeapon.insert("level", 0);
        expectedWeapon.insert("infusionType", 0);
        weapons.replace(weaponIndex, expectedWeapon);
        expected.editCharacter(1).character.insert("inventoryWeapons", weapons);
        QCOMPARE(readFile(paths.primary()), expected.serialize());
        QVERIFY2(selectProperties(JsonParser::characters(), 1, ArrayKind::Weapon,
                                  weaponId, selected, error), qPrintable(error));
        const auto outside = expected.serialize() + " ";
        QVERIFY(put(paths.primary(), outside));
        QVERIFY(!applyProperties(1, selected, edit, error, testRules));
        QCOMPARE(readFile(paths.primary()), outside);
        QCOMPARE(readFile(paths.gameBackup()), QByteArray("game backup"));
        QCOMPARE(readFile(paths.legacyEditorBackup()), QByteArray("legacy backup"));
        JsonParser::invalidate();
    }

    void propertyReadOnlyAndInvalidStates()
    {
        QString error;
        SaveDocument doc;
        QVERIFY2(SaveDocument::parse(fixture("SaveData.txt"), doc, error), qPrintable(error));
        auto slot = doc.characters().at(1);
        const auto original = slot;
        const auto item = objByGuid(slot.character, ArrayKind::Generic, "MessageSign");
        PropertySelection selected;
        QVERIFY2(selectProperties(doc.characters(), 1, ArrayKind::Generic,
                                  item.value("instanceIDOnCreation").toString(), selected, error), qPrintable(error));
        const auto options = propertyOptions(slot, selected.item);
        QVERIFY(options.readOnlyReason.isEmpty());
        QVERIFY(!options.usesReadOnlyReason.isEmpty());
        QVERIFY(options.hasSeenReadOnlyReason.isEmpty());
        PropertyEdits edit;
        edit.uses = 0;
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        edit.uses = -2;
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        edit = {}; edit.level = 0;
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        QCOMPARE(slot.character, original.character);
        edit = {}; edit.hasSeen = true;
        QVERIFY2(mutateProperties(slot, 1, selected, edit, error), qPrintable(error));
        const auto after = objByGuid(slot.character, ArrayKind::Generic, "MessageSign");
        QCOMPARE(after.value("uses").toInt(), -1);
        QCOMPARE(after.value("hasSeen").toBool(), true);
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error)); // stale
        slot = original;
        slot.character.insert("guid", "different-character");
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        slot = original;
        slot.character.insert("rememberedGUIDs", QJsonArray{"MessageSign"});
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        slot = original;
        slot.character.insert("hooveredGUIDs", QJsonArray{"MessageSign"});
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        slot = original;
        slot.character.insert("newGamePlusCount", 1);
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        slot = original;
        slot.character.remove("rememberedGUIDs");
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        slot = original;
        const QList<PropertyRule> invalidRules {
            {ArrayKind::Generic, "MessageSign", {{-1, false}, {5, true}}, {}, "invalid sentinel mixing"}
        };
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error, invalidRules));
        const auto unknown = objByGuid(slot.character, ArrayKind::Generic, "HealthPotion");
        // HealthPotion is intentionally unreviewed: its upgrade semantics are special.
        QVERIFY2(selectProperties(doc.characters(), 1, ArrayKind::Generic,
                                  unknown.value("instanceIDOnCreation").toString(), selected, error), qPrintable(error));
        QVERIFY(!propertyOptions(slot, selected.item).readOnlyReason.isEmpty());
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        QCOMPARE(slot.character, original.character);
        // Known weapon with only the seen state qualified. It cannot be
        // upgraded or infused using the built-in rule.
        const auto known = objByGuid(slot.character, ArrayKind::Weapon, "PlotThreadScissors_Dagger");
        QVERIFY2(selectProperties(doc.characters(), 1, ArrayKind::Weapon,
                                  known.value("instanceIDOnCreation").toString(), selected, error), qPrintable(error));
        const auto knownOptions = propertyOptions(slot, selected.item);
        QVERIFY(knownOptions.readOnlyReason.isEmpty());
        QVERIFY(!knownOptions.levelReadOnlyReason.isEmpty());
        QVERIFY(!knownOptions.infusionReadOnlyReason.isEmpty());
        QVERIFY(knownOptions.hasSeenReadOnlyReason.isEmpty());
        edit = {}; edit.infusionType = 2;
        QVERIFY(!mutateProperties(slot, 1, selected, edit, error));
        edit = {}; edit.hasSeen = !known.value("hasSeen").toBool();
        QVERIFY2(mutateProperties(slot, 1, selected, edit, error), qPrintable(error));
        auto seenExpected = known;
        seenExpected.insert("hasSeen", *edit.hasSeen);
        QCOMPARE(objByGuid(slot.character, ArrayKind::Weapon, "PlotThreadScissors_Dagger"), seenExpected);
        slot = original;
        auto arbitrary = slot.character.value("inventory").toArray();
        auto custom = arbitrary.first().toObject();
        custom.insert("GUID", "Unknown_Future_Item");
        custom.insert("instanceIDOnCreation", "unknown-id");
        arbitrary.append(custom);
        slot.character.insert("inventory", arbitrary);
        QVERIFY2(selectProperties(QList<SaveCharacter>{slot}, 0, ArrayKind::Generic,
                                  "unknown-id", selected, error), qPrintable(error));
        const auto unknownBefore = slot.character;
        QVERIFY(!mutateProperties(slot, 0, selected, edit, error));
        QCOMPARE(slot.character, unknownBefore);
        slot = original;
        QVERIFY(!selectProperties(doc.characters(), -1, ArrayKind::Generic, "missing", selected, error));
        QVERIFY(!selectProperties(doc.characters(), 1, ArrayKind::Generic, "missing", selected, error));
        slot.character.insert("inventory", QJsonArray{});
        slot.character.insert("inventoryWeapons", QJsonArray{});
        QVERIFY(!selectProperties(QList<SaveCharacter>{slot}, 0, ArrayKind::Generic, "missing", selected, error));
    }

    void failuresAndLegacy()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString error;
        ItemCatalog::Catalog catalog;
        QVERIFY2(testCatalog(temp.path(), catalog, error), qPrintable(error));
        SaveDocument doc;
        QVERIFY2(SaveDocument::parse(fixture("SaveData1.txt"), doc, error), qPrintable(error));
        const auto original = doc.characters().first();
        auto slot = original;
        qsizetype added = -1;
        for (const auto *guid : {"Unknown", "TESTARMOR", "BaseSword", "UnreviewedSword", "TestPotion", "TestDlcArmor"}) {
            QVERIFY(!addOne(slot, catalog, QLatin1String(guid), added, error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(added, 0);
            QCOMPARE(slot.character, original.character);
        }
        ItemCatalog::Catalog unreviewed;
        QVERIFY2(testCatalog(temp.path(), unreviewed, error, false), qPrintable(error));
        QVERIFY(!addAllMissing(slot, unreviewed, {Category::Armor}, added, error));
        QCOMPARE(slot.character, original.character);
        QVERIFY(!addAllMissing(slot, catalog, {Category::Armor, Category::Other}, added, error));
        QCOMPARE(slot.character, original.character);

        // Invalid arrays, entries, schema and ambiguous IDs cannot be
        // silently converted into empty arrays or overwritten.
        QList<QJsonValue> invalidArrays {
            QJsonValue(QJsonValue::Null), QJsonObject{{"not", "array"}},
            QJsonArray{17}, QJsonArray{QJsonObject{{"GUID", "truncated"}}}
        };
        auto dup = original.character.value("inventoryWeapons").toArray();
        dup.append(dup.first());
        invalidArrays.append(dup);
        auto fractional = original.character.value("inventoryWeapons").toArray();
        auto wrong = fractional.first().toObject();
        wrong.insert("level", 1.5);
        fractional.replace(0, wrong);
        invalidArrays.append(fractional);
        for (const auto &invalid : invalidArrays) {
            slot = original;
            slot.character.insert("inventoryWeapons", invalid);
            const auto bad = slot.character;
            QVERIFY(!addOne(slot, catalog, "TestArmor", added, error));
            QCOMPARE(added, 0);
            QCOMPARE(slot.character, bad);
        }
        slot = original;
        slot.character.remove("inventory");
        const auto bad = slot.character;
        QVERIFY(!addOne(slot, catalog, "TestSword", added, error));
        QCOMPARE(slot.character, bad);

        slot = original;
        const auto weapons = slot.character.value("inventoryWeapons").toArray();
        const auto generic = slot.character.value("inventory").toArray();
        QVERIFY2(addLegacyMissing(slot, LegacyGroup::Spells, added, error), qPrintable(error));
        QCOMPARE(added, 17);
        QVERIFY2(addLegacyMissing(slot, LegacyGroup::Runes, added, error), qPrintable(error));
        QCOMPARE(added, 25);
        const auto legacy = slot.character;
        QCOMPARE(legacy.value("inventoryWeapons").toArray(), weapons);
        QCOMPARE(legacy.value("inventory").toArray().size(), generic.size() + 42);
        for (qsizetype i = 0; i < generic.size(); ++i)
            QCOMPARE(legacy.value("inventory").toArray().at(i), generic.at(i));
        QVERIFY2(addLegacyMissing(slot, LegacyGroup::Spells, added, error), qPrintable(error));
        QCOMPARE(added, 0);
        QVERIFY2(addLegacyMissing(slot, LegacyGroup::Runes, added, error), qPrintable(error));
        QCOMPARE(added, 0);
        QCOMPARE(slot.character, legacy);
    }
};
QTEST_GUILESS_MAIN(InventoryTests)
#include "inventory_tests.moc"
