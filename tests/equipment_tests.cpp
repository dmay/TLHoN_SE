#include "equipmentservice.h"
#include "jsonparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace Equipment;

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

void addCopy(SaveCharacter &character, Inventory::ArrayKind kind,
             const QString &guid, const QString &id)
{
    const auto key = Inventory::arrayKey(kind);
    auto items = character.character.value(key).toArray();
    auto item = items.first().toObject();
    item.insert("GUID", guid);
    item.insert("instanceIDOnCreation", id);
    item.insert("futureField", QJsonObject{{"keep", true}});
    items.append(item);
    character.character.insert(key, items);
}

bool testCatalog(const QString &dir, ItemCatalog::Catalog &catalog, QString &error)
{
    // These are synthetic definitions/reviews, not a real game allow-list.
    const QJsonArray paths{
        "Assets/Design/GameplayData/Items/Relics/TestRing.asset",
        "Assets/Design/GameplayData/Items/Relics/TestRing2.asset",
        "Assets/Design/GameplayData/Items/Consumables/TestPotion.asset",
        "Assets/Design/GameplayData/Items/Consumables/UnreviewedPotion.asset",
        "Assets/Design/GameplayData/Items/Armour/TestArmor.asset",
        "Assets/Design/GameplayData/Items/Weapons/Swords/TestSword.asset",
        "Assets/Design/GameplayData/Items/Shields/TestShield.asset",
        "Assets/Design/GameplayData/Items/Weapons/Swords/UnreviewedSword.asset"
    };
    const QString path = dir + "/catalog.json";
    if (!put(path, QJsonDocument(QJsonObject{{"m_InternalIds", paths}}).toJson())) return false;
    const ItemCatalog::Source source{path, "synthetic"};
    if (!ItemCatalog::load(source, catalog, error)) return false;
    QJsonArray rows;
    const auto addReview = [&](const QString &guid, const QStringList &eligible) {
        const auto *entry = catalog.find(guid);
        QJsonObject row{
            {"guid", guid}, {"sourcePath", entry->sourcePaths.first()},
            {"category", ItemCatalog::categoryName(entry->category)},
            {"status", "equip"}, {"dlc", "base"}, {"evidence", "Synthetic equipment review only"},
            {"displayName", guid + " name"}
        };
        if (entry->category == ItemCatalog::Category::Armor) row.insert("slot", eligible.first());
        else row.insert("equipmentSlots", QJsonArray::fromStringList(eligible));
        rows.append(row);
    };
    addReview("TestRing", {"ring"});
    addReview("TestRing2", {"ring"});
    addReview("TestPotion", {"consumable"});
    addReview("TestArmor", {"helm"});
    addReview("TestSword", {"rightHand", "leftHand"});
    addReview("TestShield", {"leftHand"});
    const QString review = dir + "/review.json";
    if (!put(review, QJsonDocument(QJsonObject{{"schemaVersion", 1},
            {"catalogSha256", catalog.sha256}, {"entries", rows}}).toJson())) return false;
    return ItemCatalog::load(source, catalog, error, review);
}
} // namespace

class EquipmentTests : public QObject
{
    Q_OBJECT
private slots:
    void fixtureResolution()
    {
        ItemCatalog::Catalog catalog;
        QString error;
        for (const auto *name : {"SaveData.txt", "SaveData1.txt"}) {
            const auto bytes = fixture(QLatin1String(name));
            SaveDocument doc;
            QVERIFY2(SaveDocument::parse(bytes, doc, error), qPrintable(error));
            for (const auto &character : doc.characters()) {
                Model model;
                QVERIFY2(Equipment::read(character, catalog, model, error), qPrintable(error));
                QCOMPARE(model.entries.size(), 15);
                for (const auto &entry : model.entries)
                    QVERIFY2(entry.editable(), qPrintable(entry.diagnostic));
                QCOMPARE(model.rightHandIndex, character.character.value("rightHandEquipIndex").toInt());
                QCOMPARE(model.leftHandIndex, character.character.value("leftHandEquipIndex").toInt());
                QCOMPARE(model.consumableIndex, character.character.value("selectedConsumableIndex").toInt());
            }
            QCOMPARE(doc.serialize(), bytes);
        }
        SaveDocument doc;
        QVERIFY(SaveDocument::parse(fixture("SaveData1.txt"), doc, error));
        auto character = doc.characters().first();
        Model model;
        QVERIFY(Equipment::read(character, catalog, model, error));
        QCOMPARE(model.find({Kind::Helm})->reference, QString("Starting_Knight_Helm_OnSpawn"));
        QCOMPARE(model.find({Kind::Helm})->guid, QString("Starting_Knight_Helm"));
        QCOMPARE(model.find({Kind::RightHand, 0})->guid, QString("AvatarBaseSword"));
        QCOMPARE(model.find({Kind::LeftHand, 0})->guid, QString("AvatarBaseShield"));
        QCOMPARE(model.find({Kind::LeftHand, 1})->reference, QString("Base_Punch"));
        QCOMPARE(model.find({Kind::Consumable, 0})->reference, QString("StartHeroIconGUID"));
        QCOMPARE(model.find({Kind::Consumable, 0})->guid, QString("HealthPotion"));
        QCOMPARE(model.find({Kind::Ring, 0})->status, Status::Empty);

        addCopy(character, Inventory::ArrayKind::Weapon, "AvatarBaseSword", "second-sword");
        QList<Slot> options;
        QVERIFY(choices(character, catalog, {Kind::RightHand, 1}, options, error));
        QCOMPARE(options.size(), 3);
        QCOMPARE(options.at(1).guid, options.at(2).guid);
        QVERIFY(options.at(1).reference != options.at(2).reference);
        Changes edit;
        edit.selections.append({{Kind::RightHand, 1}, "second-sword"});
        edit.rightHandIndex = 1;
        const auto old = character.character;
        QVERIFY2(apply(character, catalog, edit, error), qPrintable(error));
        QVERIFY(Equipment::read(character, catalog, model, error));
        QCOMPARE(model.find({Kind::RightHand, 1})->reference, QString("second-sword"));
        QCOMPARE(model.find({Kind::RightHand, 1})->guid, QString("AvatarBaseSword"));
        QCOMPARE(model.rightHandIndex, 1);
        QCOMPARE(character.character.value("inventoryWeapons"), old.value("inventoryWeapons"));
        QCOMPARE(character.character.value("initRightHandGUID"), old.value("initRightHandGUID"));
    }

    void checkedTransaction()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ItemCatalog::Catalog catalog;
        QString error;
        QVERIFY2(testCatalog(temp.path(), catalog, error), qPrintable(error));
        QVERIFY(catalog.bulkEligible(ItemCatalog::Category::Armor, true).isEmpty());
        QVERIFY(catalog.bulkEligible(ItemCatalog::Category::Weapon, true).isEmpty());
        QVERIFY(catalog.bulkEligible(ItemCatalog::Category::Other, true).isEmpty());
        SaveDocument original;
        QVERIFY2(SaveDocument::parse(fixture("SaveData.txt"), original, error), qPrintable(error));
        auto &character = original.editCharacter(1);
        using A = Inventory::ArrayKind;
        addCopy(character, A::Generic, "TestArmor", "armor-id");
        addCopy(character, A::Generic, "TestRing", "ring-instance");
        addCopy(character, A::Generic, "TestRing", "second-ring-instance");
        addCopy(character, A::Generic, "TestRing2", "other-ring-instance");
        addCopy(character, A::Generic, "TestPotion", "potion-id");
        addCopy(character, A::Generic, "UnreviewedPotion", "unknown-potion");
        addCopy(character, A::Weapon, "TestSword", "sword-id");
        addCopy(character, A::Weapon, "TestShield", "shield-id");
        addCopy(character, A::Weapon, "UnreviewedSword", "unknown-sword");
        const SavePaths paths(temp.path());
        const auto before = original.serialize();
        QVERIFY(put(paths.primary(), before));
        QVERIFY(put(paths.gameBackup(), "companion"));
        QVERIFY(put(paths.legacyEditorBackup(), "legacy"));
        JsonParser::configure(paths);
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QList<Slot> options;
        QVERIFY2(choices(character, catalog, {Kind::Ring, 0}, options, error), qPrintable(error));
        QCOMPARE(options.size(), 3); // sentinel + two distinct definitions, not three instances
        QCOMPARE(options.at(1).reference, QString("TestRing"));
        QCOMPARE(options.at(1).displayName, QString("TestRing name"));

        Changes edits;
        edits.selections = {
            {{Kind::Helm}, "armor-id"},
            {{Kind::Torso}, "NULL"},
            {{Kind::RightHand, 1}, "sword-id"},
            {{Kind::LeftHand, 0}, "shield-id"},
            {{Kind::LeftHand, 1}, "Base_Punch"},
            {{Kind::Ring, 0}, "TestRing"},
            {{Kind::Ring, 1}, "TestRing2"},
            {{Kind::Consumable, 3}, "potion-id"}
        };
        edits.rightHandIndex = 1;
        edits.leftHandIndex = 0;
        edits.consumableIndex = 3;
        QVERIFY2(commit(1, catalog, edits, error), qPrintable(error));
        auto expected = original;
        auto &object = expected.editCharacter(1).character;
        object.insert("equipedHelm", "armor-id");
        object.insert("equipedTorso", "NULL");
        auto right = object.value("slottedRightHands").toArray();
        right.replace(1, "sword-id");
        object.insert("slottedRightHands", right);
        object.insert("slottedLeftHands", QJsonArray{"shield-id", "Base_Punch"});
        object.insert("equipedRings", QJsonArray{"TestRing", "TestRing2"});
        object.insert("equipedConsumables", QJsonArray{"StartHeroIconGUID", "NULL", "NULL", "potion-id", "NULL"});
        object.insert("rightHandEquipIndex", 1);
        object.insert("leftHandEquipIndex", 0);
        object.insert("selectedConsumableIndex", 3);
        const auto after = readFile(paths.primary());
        QCOMPARE(after, expected.serialize());
        QCOMPARE(readFile(paths.gameBackup()), QByteArray("companion"));
        QCOMPARE(readFile(paths.legacyEditorBackup()), QByteArray("legacy"));
        QVERIFY2(JsonParser::init(error), qPrintable(error));
        QCOMPARE(JsonParser::characters().at(1).character, object);
        Model model;
        QVERIFY2(Equipment::read(JsonParser::characters().at(1), catalog, model, error), qPrintable(error));
        QCOMPARE(model.find({Kind::Ring, 0})->guid, QString("TestRing"));
        QCOMPARE(model.find({Kind::Ring, 0})->reference, QString("TestRing"));
        QCOMPARE(model.find({Kind::Consumable, 3})->guid, QString("TestPotion"));

        // Every invalid batch includes a valid first edit; no partial commit.
        QList<Inventory::Instance> otherItems;
        QVERIFY(Inventory::read(original, 0, otherItems, error));
        QString otherHelm;
        for (const auto &item : otherItems)
            if (item.guid == "MasterChefArmor_Helm") otherHelm = item.instanceId;
        QVERIFY(!otherHelm.isEmpty());
        QList<Inventory::Instance> currentItems;
        QVERIFY(Inventory::read(original, 1, currentItems, error));
        QVERIFY(!Inventory::find(currentItems, A::Generic, otherHelm));
        const QList<Selection> rejected{
            {{Kind::Helm}, "TestArmor"}, // definition GUID, not instance ID
            {{Kind::Helm}, "not-owned"},
            {{Kind::Helm}, "StartHeroIconGUID"},
            {{Kind::Torso}, "armor-id"},
            {{Kind::Helm}, "sword-id"},
            {{Kind::Ring, 0}, "ring-instance"},
            {{Kind::Ring, 1}, "TestRing"}, // duplicate definition, despite two owned instances
            {{Kind::Ring, 0}, "TestPotion"},
            {{Kind::Consumable, 3}, "unknown-potion"},
            {{Kind::Consumable, 3}, "armor-id"},
            {{Kind::Consumable, 4}, "potion-id"}, // duplicate instance
            {{Kind::RightHand, 0}, "shield-id"}, // only reviewed for left
            {{Kind::RightHand, 0}, "unknown-sword"},
            {{Kind::RightHand, 0}, "NULL"},
            {{Kind::Ring, 0}, "Base_Punch"},
            {{Kind::LeftHand, 1}, "sword-id"}, // duplicate instance across hands
            {{Kind::Consumable, 5}, "NULL"},
            {{Kind::Ring, -1}, "NULL"},
            // Valid owned item of another character, not this one.
            {{Kind::Helm}, otherHelm}
        };
        for (const auto &invalid : rejected) {
            Changes bad;
            bad.selections = {{{Kind::Arms}, "NULL"}, invalid};
            QVERIFY2(!commit(1, catalog, bad, error), qPrintable(invalid.reference));
            QVERIFY(!error.isEmpty());
            QCOMPARE(readFile(paths.primary()), after);
            QCOMPARE(JsonParser::characters().at(1).character, object);
        }
        for (int invalid : {-1, 2, 100}) {
            Changes bad;
            bad.selections = {{{Kind::Helm}, "NULL"}};
            bad.leftHandIndex = invalid;
            QVERIFY(!commit(1, catalog, bad, error));
            QCOMPARE(readFile(paths.primary()), after);
        }
        Changes bad;
        bad.consumableIndex = 5;
        QVERIFY(!commit(1, catalog, bad, error));
        bad = {};
        bad.rightHandIndex = 2;
        QVERIFY(!commit(1, catalog, bad, error));
        QVERIFY(!commit(-1, catalog, edits, error));
        QVERIFY(!commit(2, catalog, edits, error));
        QCOMPARE(readFile(paths.primary()), after);

        // Swap two ring definitions in one batch. Unequip an active hand and
        // consumable without changing their active indices (empty is observed).
        Changes swap;
        swap.selections = {{{Kind::Ring, 0}, "TestRing2"}, {{Kind::Ring, 1}, "TestRing"},
                           {{Kind::RightHand, 1}, "Base_Punch"}, {{Kind::Consumable, 3}, "NULL"}};
        QVERIFY2(commit(1, catalog, swap, error), qPrintable(error));
        object.insert("equipedRings", QJsonArray{"TestRing2", "TestRing"});
        right.replace(1, "Base_Punch");
        object.insert("slottedRightHands", right);
        object.insert("equipedConsumables", QJsonArray{"StartHeroIconGUID", "NULL", "NULL", "NULL", "NULL"});
        const auto swapped = readFile(paths.primary());
        QCOMPARE(swapped, expected.serialize());

        // Explicit no-op must not rewrite, and stale-save guard remains in effect.
        QFile file(paths.primary());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromSecsSinceEpoch(1577836800, Qt::UTC), QFileDevice::FileModificationTime));
        file.close();
        const auto time = QFileInfo(paths.primary()).lastModified();
        QVERIFY2(commit(1, catalog, swap, error), qPrintable(error));
        QCOMPARE(QFileInfo(paths.primary()).lastModified(), time);
        QVERIFY(put(paths.primary(), swapped + "\n"));
        QVERIFY(!commit(1, catalog, edits, error));
        QCOMPARE(readFile(paths.primary()), swapped + "\n");
        QCOMPARE(readFile(paths.gameBackup()), QByteArray("companion"));
        QCOMPARE(readFile(paths.legacyEditorBackup()), QByteArray("legacy"));
        JsonParser::invalidate();
    }

    void unknownAndMalformed()
    {
        ItemCatalog::Catalog catalog;
        QString error;
        SaveDocument doc;
        QVERIFY(SaveDocument::parse(fixture("SaveData1.txt"), doc, error));
        const auto original = doc.characters().first();
        auto character = original;
        addCopy(character, Inventory::ArrayKind::Generic, "FutureArmor_Helm", "unknown-owned");
        character.character.insert("equipedHelm", "unknown-owned");
        character.character.insert("equipedLegs", "missing-id");
        character.character.insert("equipedRings", QJsonArray{"not-owned-ring", "NULL"});
        character.character.insert("slottedRightHands", QJsonArray{"Base_Punch", "unknown-hand"});
        const auto before = character.character;
        Model model;
        QVERIFY2(Equipment::read(character, catalog, model, error), qPrintable(error));
        QCOMPARE(model.find({Kind::Helm})->status, Status::Unverified);
        QCOMPARE(model.find({Kind::Helm})->guid, QString("FutureArmor_Helm"));
        QCOMPARE(model.find({Kind::Helm})->reference, QString("unknown-owned"));
        QCOMPARE(model.find({Kind::Legs})->status, Status::Dangling);
        QCOMPARE(model.find({Kind::Ring, 0})->status, Status::Dangling);
        QCOMPARE(model.find({Kind::RightHand, 1})->status, Status::Dangling);
        QList<Slot> options;
        QVERIFY(!choices(character, catalog, {Kind::Helm}, options, error));
        QVERIFY(options.isEmpty());
        Changes bad;
        bad.selections = {{{Kind::Helm}, "NULL"}};
        QVERIFY(!apply(character, catalog, bad, error));
        bad = {};
        bad.rightHandIndex = 1;
        QVERIFY(!apply(character, catalog, bad, error));
        QCOMPARE(character.character, before);

        // An unrelated valid edit must preserve every read-only reference.
        Changes good;
        good.selections = {{{Kind::Arms}, "NULL"}};
        QVERIFY2(apply(character, catalog, good, error), qPrintable(error));
        auto expected = before;
        expected.insert("equipedArms", "NULL");
        QCOMPARE(character.character, expected);

        for (const auto &malformed : QList<QPair<QString, QJsonValue>>{
                 {"equipedHelm", QJsonValue::Null},
                 {"equipedRings", QJsonArray{"NULL"}},
                 {"slottedLeftHands", QJsonArray{"Base_Punch", "Base_Punch", "Base_Punch"}},
                 {"equipedConsumables", QJsonArray{"NULL", "NULL", 3, "NULL", "NULL"}},
                 {"rightHandEquipIndex", 0.5},
                 {"leftHandEquipIndex", -1},
                 {"selectedConsumableIndex", 5},
                 {"rightHandEquipIndex", "1"},
                 {"inventory", QJsonValue::Null}}) {
            character = original;
            character.character.insert(malformed.first, malformed.second);
            const auto invalid = character.character;
            QVERIFY(!Equipment::read(character, catalog, model, error));
            QVERIFY(model.entries.isEmpty());
            QVERIFY(!apply(character, catalog, good, error));
            QCOMPARE(character.character, invalid);
        }

        character = original;
        const QJsonValue sword = character.character.value("slottedRightHands").toArray().first();
        character.character.insert("slottedRightHands", QJsonArray{sword, sword});
        QVERIFY(Equipment::read(character, catalog, model, error));
        QCOMPARE(model.find({Kind::RightHand, 0})->status, Status::Invalid);
        QCOMPARE(model.find({Kind::RightHand, 1})->status, Status::Invalid);
        character = original;
        character.character.insert("equipedArms", character.character.value("equipedTorso"));
        QVERIFY(Equipment::read(character, catalog, model, error));
        QCOMPARE(model.find({Kind::Arms})->status, Status::Invalid);
        QVERIFY(!model.find({Kind::Torso})->editable());
    }
};

QTEST_GUILESS_MAIN(EquipmentTests)
#include "equipment_tests.moc"
