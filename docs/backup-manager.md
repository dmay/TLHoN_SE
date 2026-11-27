# Backup Manager (DIG-120)

Open **Settings → Backup Manager** even when no valid character/save is
loaded. The catalog lists local timestamps, names, source paths, kind, byte
size and verification status. Select a row for exact ID, source profile,
file names, sizes, SHA-256 checksums, missing/not-captured states and diagnostics.
Integrity verification and primary save-format validation are separate.

- **Create backup:** close the game and wait for Steam Cloud sync, then enter an
  optional label. This captures the entire valid save, including the exact
  companion-file presence state. The new snapshot is selected immediately.
- **Edit label / Delete selected:** act only on a selected verified, non-safety
  snapshot; deletion requires explicit confirmation and does not touch the
  active save. Invalid or unidentified snapshots are retained for inspection.
- **Restore selected:** review the confirmed full-save replacement, or legacy
  primary-only scope, then confirm. See [restore and recovery](restore-recovery.md).
  Even an absent/corrupt active save allows restore from a parseable snapshot.
  A verified but unparseable primary is not offered for automatic restore.
  Automatic safety entries cannot be renamed/deleted.
- **Import legacy backup:** copies the old editor's SaveData.txt_bckp without
  changing the source. A missing/corrupt active save does not prevent recovery
  via import.
- **Choose backup folder:** switches the catalog; does not move old snapshots.
  The choice is persisted. Open backup/save folders to inspect their contents.
- **Reload save:** reruns interrupted-restore recovery, loads the active file
  and refreshes the UI. Refresh list just inspects the catalog, preserving a
  selected snapshot ID where possible. It never edits a save or manifest.

Mutating actions are blocked if the Windows process check fails or the game
runs. Ordinary character edits additionally require the loaded primary bytes
still match disk. Normal capture and metadata mutations require a valid
active primary; recovery exceptions are restore and legacy import. Pending
restore journals block all catalog mutations; use Reload save to recover.
The game and Steam do not honor editor locks; close the game throughout
operations and use the documented Steam Cloud precautions.

## UI refresh contract for inventory/equipment

Tab_Settings loads the new character list with combo signals blocked and
preserves the previous slot index when valid, or selects slot 0 / -1. It emits
characterChanged and saveRestored after reloading; editingAvailabilityChanged
notifies of changes in editability (e.g. running game, changed disk bytes).
New character-dependent tabs should subscribe to these signals in initSettings,
validate the selected index against JsonParser::characters(), clear stale
fields on no selection, and block widget signals during programmatic updates.
Use JsonParser::mutateCharacter for edits; never write in a refresh handler.
Equipment and Inventory remain placeholders until their implementation issues.

Tests use QTemporaryDir fixtures, a stubbed catalog backend for failure and
availability states, and the actual Settings entry point with modal dialog
responses driving create, rename, reopen, delete, cancellation, and restore.
No installed game or active save is needed.
