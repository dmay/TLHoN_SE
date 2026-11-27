# Owned inventory browser (DIG-127)

Open the Inventory tab and choose **Owned items**; the **Add items** page
retains the reviewed catalog picker and grant controls from DIG-124.
Browsing does not require a game catalog. Loading one on Add items provides
display names and catalog category metadata; entries missing from the catalog
fall back to their exact definition GUID and the Other category. Filter
by name, GUID, instance ID, persisted array, category, and saved flag state.
Each record is a separate row, including duplicates; Copies is the total
number of matching definitions in the same array. IDs are exact and
array-scoped. Equipment status is intentionally not inferred from a GUID or
item name (DIG-125/DIG-128 own equipment semantics).

Select a row to see its complete saved record and proposed state side by
side. Identity, unknown fields, and unsupported fields are read-only, with
reasons and full values in cell tooltips. The native state combo offers only
complete typed state tuples returned by the DIG-123 property service, so
changing a count that also requires a seen-state change is presented
explicitly as a single batch. No free-form numeric input or inferred enum
values are accepted. Apply commits once; Cancel only discards the pending
selection. The detail stays bound to its selected instance when a filter
hides its row. A different row, character, reload, or confirmed restore
asks before discarding a pending draft. Context changes and stale records
are checked before applying; saves are protected by the existing checked
transaction. Other records, equipment references, backup files and opaque
sections are preserved.

The built-in candidate rules are based on fixtures, **not** evidence of
in-game transitions. In particular, no nonzero weapon upgrade/infusion
alternative is currently available. The browser will expose additional
approved combinations automatically when the typed service rules are
qualified and extended. See [inventory properties](inventory-properties.md)
and DIG-129 for game qualification. A missing or malformed inventory is
reported without any attempted repair or write.

The editor UI test uses a temporary directory and the repository fixtures.
It exercises both arrays, identical ID text in different arrays, duplicate
definition instances, metadata/search/filter, unknown nested properties,
complete-state selection, cancel and context veto, invalid target and
cooperating-editor lock rejection, exact envelope preservation, supported
generic and weapon seen-state edits, reload and restore, external changes,
and malformed/missing save states. The catalog grant UI test verifies that
newly added items appear in the owned list and that the existing picker
continues to work. No active save or game installation is accessed.
