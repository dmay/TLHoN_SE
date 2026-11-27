# Equipment service (DIG-125)

This service reads and validates equipment independently of widgets. DIG-128
owns enabling and wiring the existing Equipment tab. No inventory grants,
item-property edits, or initialization GUID changes are performed here.

## Observed schema

The following is based on exact reference resolution of all three characters
in the two committed fixtures, plus the local-save observations in DIG-125.
These files do not establish all game rules.

| Saved field | Stored value | Observed capacity | Empty sentinel |
| --- | --- | --- | --- |
| `equipedHelm`, `equipedTorso`, `equipedArms`, `equipedLegs` | inventory instanceIDOnCreation | one each | `NULL` |
| `slottedRightHands`, `slottedLeftHands` | inventoryWeapons instanceIDOnCreation | two each | `Base_Punch` |
| `equipedRings` | owned inventory definition GUID (local-save evidence) | two | `NULL` |
| `equipedConsumables` | inventory instanceIDOnCreation | five | `NULL` |
| `rightHandEquipIndex`, `leftHandEquipIndex` | zero-based integer | 0–1 | none |
| `selectedConsumableIndex` | zero-based integer | 0–4 | none |

The starter fixture has the four Starting_Knight armor definitions equipped;
the second character of the larger fixture has four MasterChefArmor pieces
equipped. AvatarBaseSword is observed on the right; AvatarBaseShield,
OldWoodenCultShield_StandardShield, and Torch are observed on the left.
HealthPotion resolves via the non-UUID instance ID StartHeroIconGUID in
the first quick slot. The first character in the larger fixture has
every slot empty but left-hand and consumable index 1. Empty active slots
are therefore supported; unequipping leaves an index unchanged unless
the user explicitly edits it. No auto-fill/compaction of arrays.

Both fixtures have empty rings; the non-empty reference format is from
the observed local save recorded in DIG-125, not verified by a repository
non-empty ring fixture. The tests use synthetic reviewed definitions to
exercise it. Same-definition duplicate ring slots and repeated use of the
same instance within its owning array's equipment slots are conservatively
disallowed; distinct instances of a duplicate weapon GUID remain selectable.
Positive proof of all duplicate/equip rules, cross-hand weapon/shield
eligibility, consumable/spell/relic subtype eligibility, and any other slot
counts or sentinels needs a disposable game profile (DIG-130).

## Eligibility and identity

The read model uses the shared `Inventory::read` and array-scoped unique
instance lookup. It always retains the exact string reference, definition
GUID (if resolvable) and display name (reviewed catalog name or GUID fallback).
A ring resolves by exact GUID in the generic array, not by instance ID.
An unresolved catalog name is never sufficient evidence of slot compatibility.

Explicit exact GUID/slot pairs observed equipped in the repository fixtures
are recognized, even without a catalog. No suffix-based matching (including
_Arms or _Helm), broad weapon classification or consumable-name heuristic is
used. To extend the set, load the reviewed catalog for the matching build
and provide explicit eligibility metadata; exclusions, category mismatches
and ambiguous paths block eligibility. Explicit review of slot compatibility
takes precedence over the repository observation. Equipment selection
requires that the selected character already owns the reference; it does
not grant an item or authorize a DLC purchase/unlock.

The existing hash-bound catalog review supports an additional optional
`equipmentSlots` field and `status: "equip"` (equipment-only; no bulk-grant
eligibility). Existing `status: "allow"` rows for weapons and shields may
optionally specify `equipmentSlots`. For armor use the existing verified
`slot`; for weapon/shield use `["leftHand"]`, `["rightHand"]` or both,
depending on independent verification. For Other definitions (which are
never bulk-eligible), use exactly one of `["ring"]` or `["consumable"]`
with `status: "equip"`. Equipment-only review still requires exact
definition/source path, catalog hash, category, DLC metadata and evidence
like existing reviews. Names/types and unreviewed path hints do not
establish compatibility. Example row (illustrative only):

```json
{
  "guid": "DemoRing",
  "sourcePath": "Assets/Design/GameplayData/Items/Relics/DemoRing.asset",
  "category": "other",
  "status": "equip",
  "equipmentSlots": ["ring"],
  "dlc": "base",
  "evidence": "<actual independently verified equipment evidence>"
}
```

Unknown, dangling, excluded, duplicate and mismatched existing references
retain their exact raw values and a diagnostic; the affected slot is
read-only, not silently repaired or even cleared. Other valid slots
remain editable. Unsupported array lengths, missing/wrong-typed fields,
malformed inventory, or invalid stored active indices reject the whole
equipment model with an error; do not resize/normalize those characters.
Ring/consumable catalog review and exact hand eligibility remain incomplete
for real game definitions, and no real reviewed list is distributed.

## Integration contract

Use `Equipment::read(character, catalog, model, error)` to populate the UI.
The model has 15 entries with explicit kind and zero-based index (scalar
armor index 0), stored reference, resolved GUID and human-readable display
identity, status/diagnostic; indices are separate. On a structural error
show the error and disable editing. With a successful read use
`slot.editable()` to decide whether a slot can be edited; show the raw
unknown reference and the diagnostic for read-only slots.

Use `Equipment::choices(character, catalog, position, options, error)`
to list just eligible owned references and the explicit unset sentinel.
Disambiguate duplicate instance names by instance ID in the UI; do not
use a display string or row number as the stored reference. Choices may
include an item equipped in another slot to support atomic swaps. Apply
validates uniqueness in the final state; duplicate existing references
are shown read-only. An active index can select an empty or resolved slot,
but not an invalid or unverified one.

Stage changes in the UI, then submit once:

```cpp
Equipment::Changes changes;
changes.selections = {{{Equipment::Kind::RightHand, 1}, chosenInstanceId},
                      {{Equipment::Kind::Helm}, "NULL"}};
changes.rightHandIndex = 1;
QString error;
const bool success = Equipment::commit(selectedIndex, catalog, changes, error);
```

`Equipment::apply` is the pure checked candidate mutation and can also be
called inside a single `JsonParser::mutateCharacterChecked` callback.
`commit` calls that protected writer directly. Omitted slots and indices
are not changed; selecting the same reference is a no-op. Invalid positions,
wrong types, unowned references, duplicate requested positions, unverified
references, incompatible slots, repeated references or out-of-range
indices reject the entire batch. No automatic coupling to other indices
or slots; explicit requested changes are checked as a final consistent
state, including simultaneous swaps. The initialization GUIDs, inventory,
info, quests, other characters, settings and opaque sections are untouched.
The protected write retains the existing stale-save, game-running
(Windows), lock, recovery and atomic replacement protections.

Revalidate selected character and pending changes when applying, refresh
after commit, and never commit on view population. Cancel is simply
discarding the staged changes.

## Automated validation

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --verbose --output-on-failure
```

`equipment_tests` covers all repository characters, reference identity,
synthetic reviewed slot eligibility, exact ownership, duplicate weapon
GUIDs, ring identity/uniqueness, invalid sentinels and indices, unknown
existing entries, transactional mixed edits, no-op and stale-save rejection.
It invokes the normal protected writer with a QTemporaryDir containing
disposable fixture copies, compares bytes against the expected selected
character-only envelope update, and checks companion and legacy backups.
Catalog tests verify review compatibility and rejection of invalid equipment
metadata. This is fixture-based validation, not proof of an in-game reload.
