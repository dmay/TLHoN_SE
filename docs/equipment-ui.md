# Editing equipment (DIG-128)

Select the character in Settings and open Equipment using the existing tab button.
All four armor slots, two slots per hand, two rings, five consumables and the
three active indices are shown. The UI uses the saved reference, not the
definition name or combo-box row number. A hover tooltip shows the exact saved
identity, GUID, proposed identity and any service diagnostic. Empty choices
explicitly show the appropriate `NULL` or `Base_Punch` sentinel. Rings use
definition GUIDs; other slots use instance IDs, including duplicate weapons.

Only owned, slot-eligible items appear as choices. With no catalog loaded, only
the exact fixture-observed eligibility pairs are available. Load the matching
catalog and explicit reviewed list in Inventory to expand choices (see
[equipment service](equipment-service.md)). Unverified/dangling current
references remain displayed read-only; an unsupported model disables editing.
An empty dropdown alternative list is not permission to grant new equipment.

Select all desired changes, including active indices, then Apply once. The
preview rejects invalid combinations (e.g. the same weapon instance in two
slots), but permits staging a swap before applying. Cancel discards the whole
draft without saving. The catalog/review and selected equipment/ownership
context are checked again on Apply, and Equipment::commit performs the single
protected save write. No inventory, initialization GUIDs or unrelated save data
are rewritten by the equipment mutation. Game-running or stale-save protection
also applies. Close the game and finish Steam Cloud sync before editing.

Character selection, reload and restore ask before discarding a draft;
inventory or catalog updates rebuild the options and discard any draft with
a status notice. Language changes preserve a draft for the same context.
Unsupported armor defense/weight totals from the former mockup are not shown,
because those values have not been verified. The application uses its
existing English/Russian translation resources.

The editor UI tests use disposable fixture copies and synthetic review
metadata, not the installed game's catalog. DIG-130 remains responsible for
actual game compatibility and round-trip verification.
