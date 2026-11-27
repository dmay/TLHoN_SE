# Shared inventory model (DIG-122)

The inventory model is independent of widgets and the game installation;
it does not load a catalog or write files by itself. The new Stats
armor/weapon and Inventory picker UI belongs to DIG-124. Existing Stats
spell and rune actions already use this shared model.

## API contract

- `Inventory::read(slot, ...)` / `Inventory::read(document, index, ...)`
  return both arrays, in their original order (generic, then weapon).
  Each `Instance` contains the array kind, exact definition GUID, stable
  instance ID, and full original JSON object. Neither a catalog match nor
  a UUID-shaped **existing** ID is required. Existing starter IDs such as
  `Starting_Knight_Helm_OnSpawn` are legitimate. Unknown GUIDs/fields and
  duplicate GUIDs with distinct IDs remain intact.
- `Inventory::find(instances, kind, instanceId)` resolves a unique instance
  within that array. The character index is also part of the identity.
  Never use a definition GUID or a row index as an instance identity.
  Duplicate instance IDs within the same array are ambiguous and fail
  validation. Both arrays must exist and contain complete objects with
  string identity fields, booleans and the observed integer fields;
  malformed/unsupported shapes fail closed rather than becoming empty.
  Property ranges and editing are separate work (DIG-123).
- `addOne` and `addMissing` validate every explicit requested GUID against
  the loaded, hash-bound reviewed catalog (`Entry::bulkEligible`).
  `addAllMissing` selects the eligible definitions in all requested
  categories; use `{Category::Weapon, Category::Shield}` for the combined
  weapon action. Unsupported categories, unknown or unreviewed explicit
  requests, exclusions and unentitled DLC requests are rejected. An
  empty eligible bulk scope returns a diagnostic; a fully owned scope is
  a successful no-op. DLC eligibility defaults to false and is not an
  ownership check; the caller must establish ownership explicitly.
- Exact GUID matching is case-sensitive, scoped to the destination array.
  Existing duplicates and even misclassified existing items are never
  deleted or moved, and do not suppress a grant in the correct array.
  New UUIDs are generated without braces and checked against both arrays
  of the selected character and all new IDs in the batch.
- Operations build a detached result, never alter array positions of existing
  items, and never change equipment, storage, achievement counters,
  `rememberedGUIDs`, `hooveredGUIDs`, or other character/section data.

Use the checked transaction, not a direct file write:

```cpp
qsizetype added = 0;
QString error;
const bool success = JsonParser::mutateCharacterChecked(selectedIndex,
    [&](SaveCharacter &candidate, QString &mutationError) {
        return Inventory::addOne(candidate, reviewedCatalog, exactGuid,
                                 added, mutationError, dlcEntitled);
    }, error);
```

Only report `added` after `success`; the callback count is staged until
the outer transaction succeeds. A false callback discards the entire
detached candidate before any serialization/staging. The existing save
lock, recovery, game-process (Windows), stale bytes, checked envelope and
atomic replacement protections apply, and an unchanged candidate is not
rewritten. SaveData.txt.bak and the editor's legacy backup are untouched.
Select the character again when confirming the UI and refresh after a
successful commit; never mutate as part of rendering.

## Evidence and candidate creation defaults

Both repository fixtures were inspected. `SaveData.txt` contains two
characters with 169/61 and 94/27 generic/weapon records;
`SaveData1.txt` has one with 7/2. Armor records occur in `inventory`,
while weapons and shields occur in `inventoryWeapons`. All records in
these fixtures use the corresponding shapes:

| Field | Armor / generic | Weapon / shield |
| --- | --- | --- |
| GUID | exact definition string | exact definition string |
| instanceIDOnCreation | new UUID string | new UUID string |
| uses | integer -1 | absent |
| isAwake | false | false |
| isHoovered | false | false |
| hasSeen | true | true |
| newGamePlusCount | integer 0 | integer 0 |
| level | absent | integer 0 |
| infusionType | absent | integer 0 |

The candidate defaults match full records (excluding identity) for
`Starting_Knight_Helm`, `AvatarBaseSword` and `AvatarBaseShield` in the
small fixture and ordinary `SkeletonArmor_Helm`,
`DaggerofCycles_Dagger` and `Degraded_SmallShield` in the larger fixture.
The larger fixture also contains differing flags and deliberately unusual
upgrade/NG+ counts; these observations do **not** establish legal ranges or
a universal on-acquisition state. Existing records retain their exact
values. The legacy 17 spell/throwable and 25 rune GUIDs preserve the
previous generic `uses=-1`, flags and count behavior, even though some
game records (e.g. the throwable) differ; no newly guessed state changes.

The repository fixtures provide structural evidence, not proof that all
records were game-created or that every candidate will survive a live
reload. On an independently backed-up, explicitly authorized disposable
profile, compare an ordinary newly acquired armor piece, weapon and shield
against the candidate schema and flags; record any subtype or DLC
exceptions. Confirm add/load/re-save and preservation of other items/slots.
DIG-126 covers representative game loading, valid catalog coverage,
and entitlement qualification. No real allow-list is shipped. DIG-121's
manual prerequisite is treated as satisfied for implementation per the
user's instruction, not as new observed test evidence.

## Automated qualification

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The new `inventory_tests` uses disposable fixture copies and an invented
catalog with an invented hash-bound review; tests the selected slot,
both array schemas, exact-case GUID matching, UUIDs, unknown fields,
existing duplicate instances, wrong-array definitions, DLC gating,
malformed shapes, checked rollback, exact serialized bytes, no-op mtime,
stale input, unchanged companion and legacy files, and repeatable legacy
grants. `editor_tests` uses the existing Stats entry point for the
spell/rune regression. The separate catalog build and existing save/backup/
restore and packaging checks remain in CI.
