# Per-instance property mutations (DIG-123)

This is the shared checked mutation service; DIG-127 owns the Inventory
detail pane. The API edits exactly one persisted record in its owning
array and never creates inventory fields or exposes arbitrary JSON editing.

## Evidence and limitations

The original fixtures are not known-clean recordings of game actions.
An independent parse of all three character records (360 items) found:

| Field | Evidence | Decision |
| --- | --- | --- |
| Generic uses | -1 and nonnegative integers; exact counts vary by definition | Review exact definition/count states only. Do not infer a universal maximum, convert a finite item to -1, or refill every item. |
| hasSeen | Both booleans occur for the same generic and weapon definitions | Allow only the reviewed combined states below; no companion writes. |
| level | 0, 5, 6, 1000 and 10000 in weapons | Outlier values and mixed NG+ counts prevent deriving the maximum or eligibility from the fixtures. |
| infusionType | 0 and 2; only one nonzero occurrence | No verified enum mapping or eligibility; nonzero transitions remain unqualified. |
| isAwake | 23/20 true in DeathWhisper generic/weapon records, but only 6/18 of those GUIDs are in rememberedGUIDs | Read-only: setting the flag or reconstructing lists/counters is not established. |
| isHoovered | Five true weapon records in each large-fixture character, with empty hooveredGUIDs and zero itemsHoovered | Read-only: do not manufacture companion state or achievements. |
| newGamePlusCount | 0 plus suspicious 1000/10000/10000000 values | Read-only. No inference of meaningful legal domain. |

Community gameplay evidence is useful context, not a version-specific save
schema: the [achievement guide](https://steamcommunity.com/sharedfiles/filedetails/?id=2986780184)
describes a +5 upgrade target; a later
[DLC upgrade discussion](https://steamcommunity.com/app/1766100/discussions/0/4199112089436403520/)
reports +6. A [blank parchment discussion](https://steamcommunity.com/app/1766100/discussions/0/3788128618781592910/)
suggests a numeric infusion reset, but does not establish the full enum or
which definitions support it. A [blacksmith discussion](https://steamcommunity.com/app/1766100/discussions/0/3548301990169688270/)
also describes progression-dependent infusion availability. None shows
controlled before/after save data for the installed build.

The built-in table intentionally provides a small **fixture-backed candidate**
set, not a global range. Each state below occurs on the exact definition
with unawakened/unburdened NG0 item and character, absent from the two
companion GUID lists. The integer values are the saved values, not inferred
stats or display labels. A pair/triple is a complete allowable target (no
Cartesian product of separately observed values).

| Definition (array) | Complete observed alternatives |
| --- | --- |
| Consumable_SlowStaminaRestore (generic) | (1, false), (2, true) |
| Consumable_SneakAid (generic) | (8, false), (9, true) |
| Consumable_WepStatus_Glitch (generic) | (5, false), (6, true) |
| Consumable_StrongPotion_Health (generic) | (1, false), (8, true) |
| GenericAmmo (generic) | (10, true), (20, true) |
| MessageSign, SkeletonArmor_Helm (generic) | (-1, false), (-1, true) |
| PlotThreadScissors_Dagger (weapon) | (0, 0, false), (0, 0, true) |

Generic tuples are (uses, hasSeen); weapon tuples are
(level, infusionType, hasSeen). No built-in rule changes a weapon upgrade
or infusion. Do not label -1 as infinite without game evidence; it is a
fixed observed sentinel. Existing out-of-domain values (even if a user
previously modified them) are inspectable, not silently repaired. All other
definitions/contexts and field combinations are read-only. Fixture evidence
is sufficient for automated structural qualification, not proof that a live
game transition is valid; DIG-127 should not expose unqualified mutations as
game-verified. DIG-129 must compare actual game-created before/after states and
reload for the installed version, and extend or correct the reviewed table
only with evidence.

## API for DIG-127

Include inventoryproperties.h. Use the actual current selected character
index, not a row number or character name. Capture (slot index, character
GUID, array kind, full exact instance record) when opening an edit pane.
Use propertyOptions for the reviewed alternatives and diagnostics. A
nonempty per-field reason means read-only; offer only combinations
contained in the complete state lists. If changing one field requires a
partner field change, present that explicitly as one batch; never change
it implicitly. GUID, ID, unknown fields, isAwake, isHoovered and NG+ have
no mutation setters.

    Inventory::PropertySelection selection;
    QString error;
    if (!Inventory::selectProperties(JsonParser::characters(),
                                     selectedIndex, kind, instanceId,
                                     selection, error)) {
        // show error, don't edit
    }
    // ... display propertyOptions(currentCharacter, selection.item) ...
    Inventory::PropertyEdits edit;
    edit.uses = 2;
    edit.hasSeen = true;
    const bool success = Inventory::applyProperties(currentSelectedIndex,
                                                    selection, edit, error);
    // After success: refresh and capture a fresh selection.

This example must be used only for the matching reviewed state. Externally
changed bytes, stale item snapshot, stale selected character, missing
identity, wrong array, malformed inventory, invalid review, unreviewed
current/target state, and inconsistent companion context fail before
serialization. The app entry point invokes JsonParser::mutateCharacterChecked
and the existing protected store: game process, recovery, optimistic byte
check, editor lock and atomic replacement guards. No-op leaves bytes and
mtime unchanged. The mutation never touches any other inventory record,
position, equipped reference, character state, companion lists/counters,
other slot, opaque section, game backup or legacy editor backup. The
supplied rules are trusted reviewed C++ application data (not editable UI
input); a separate explicit rule parameter supports testing and future
version-specific review integration. Never construct the rules from an
edited JSON record, user-entered limits, or raw path/catalog presence.

## Qualification

The existing inventory_tests suite exercises the real checked facade using
QTemporaryDir fixture files. It verifies combined uses/seen changes on
two copies of the same GUID, selected slot, full expected envelope bytes,
reloaded values, no-op timestamp, stale selection and external change,
preserved equipped references/backups and unknown fields. It separately
injects an **invented test-only** weapon rule to exercise upgrade/infusion
batching, valid lower boundary, negative/large invalid numbers, invalid
enum combinations, wrong array and rejection without the test rule.
This is not a claim that those particular weapon values are legal in-game.
Unknown/empty selections, fixed sentinel, read-only flag explanations,
unreviewed NG+ and remembered/hoovered context are also exercised.

Run the repository's documented Windows CMake/CTest Release workflow.
No active saves/game installation are required or touched.
