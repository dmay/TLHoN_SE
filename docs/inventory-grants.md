# Catalog-driven grants (DIG-124)

Use a disposable copy before qualifying new item defaults. Close the game,
finish Steam Cloud synchronization, and make a verified backup in Settings.
The normal editor still targets the configured save directory; `--save-dir`
can point to a disposable copy. Neither catalog browsing nor UI refresh writes
any save data.

1. In Settings, select the target character.
2. Open Inventory. Discover the installed Steam game, or choose its
   `TLHON_Data/StreamingAssets/aa/catalog.json` (a metadata-only copy works).
   Browse the definitions by name/exact GUID, category and eligibility.
3. Choose a version-matched, independently reviewed allow-list JSON. See
   [the catalog format](item-catalog.md). Without that file the picker is
   read-only; a corrupt or stale review disables all grants. No real review
   or game catalog is shipped. Verified names are preferred; the exact GUID
   is always available in the preview and is the mutation key.
4. If you own the required DLC, explicitly select the DLC checkbox. Otherwise
   even reviewed DLC definitions stay ineligible. This is a user ownership
   assertion, not automatic Steam ownership detection. The picker displays
   category, review status, DLC state, owned instance count and destination
   (`inventory` for armor; `inventoryWeapons` for weapons and shields).
5. Select an eligible missing definition and click **Add selected item**.
   Confirm the selected character, exact item and count. No equipped items,
   upgrades or existing duplicate instances are changed. No arbitrary GUID
   can bypass the reviewed catalog.
6. Bulk grants require independent disposable-profile validation of the item
   factories ([DIG-122 candidate defaults](inventory-model.md)). After that
   qualification, explicitly check **Enable bulk adds after verifying factory
   defaults on a disposable save** in Inventory. This is a user assertion,
   not proof of live qualification. In Settings enable cheat mode; open Stats
   and expand the cheat controls. **Add all missing armor** or **Add all missing
   weapons/shields** confirms the exact missing count, already-owned eligible
   definitions, and ineligible definitions. The bulk scope is the entire
   eligible reviewed category, not the current picker search/filter. Inspect
   the confirmation details for the exact GUID list. Decline to cancel.

Catalog/review paths, DLC entitlement and bulk qualification are session-only;
loading/reloading resets both assertions. User feedback reports committed count
only after the protected write returns success. Confirmations are invalidated
by character/restore or catalog configuration changes; the catalog and review
are read again before preview and before write. The protected SaveStore also
rejects game-running, stale, locked, read-only and recovery-pending saves.
Repeated bulk grants report zero and do not write. Add-one is disabled for an
already-owned definition in the correct array. Duplicate definition instances
are counted but retained. This is not the per-instance property editor.

## Qualification

`editor_tests::catalogGrantUi` exercises the actual MainWindow Inventory and
Stats controls using a disposable copy of the repository fixture and synthetic
catalog/review, including no catalog/review, exact GUID vs search case, filters,
DLC and bulk gates, duplicate ownership, cancel, add one armor/weapon, bulk
armor and weapon/shield, repetition, selected character and changed selection,
external writer, stale catalog, reload, and unrelated byte preservation.
CI runs the Windows C++17/Qt build, CTest, standalone catalog tests and packaged
startup/close smoke. These tests do not establish live save compatibility or
real catalog coverage. DIG-126 owns real catalog review and representative
in-game reload and bulk qualification.
