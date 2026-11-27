#include "inventorymodel.h"
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
