# Restore and interruption recovery (DIG-119)

## Restore a snapshot

1. Exit The Last Hero of Nostalgaia. Wait for Steam Cloud to finish syncing;
   do not start the game or run a second editor during restoration.
2. Open Settings → Backup / recovery → Restore snapshot. Choose a verified
   snapshot (or first import the old editor's `SaveData.txt_bckp`).
3. Review the source, target, snapshot ID, exact file states, sizes and
   checksums (Show Details). Confirm only if these are the intended files.
   A full snapshot replaces the entire save/all characters. If its game
   companion `SaveData.txt.bak` was missing, the active companion is removed.
   Legacy restore replaces only `SaveData.txt`; it neither creates nor
   changes the game companion and never moves/removes the legacy source.
4. Before changing the active files, the editor creates a verified immutable
   **Automatic pre-restore safety snapshot** in the configured backup root.
   A missing or corrupt current primary is allowed in this explicit recovery
   flow: the safety snapshot preserves all existing bytes and missing states.
   If the safety capture or validation fails, restoration stops.
5. On success the editor reloads all characters and retains the prior slot
   index if still available. Restore the safety snapshot like a full snapshot
   to undo the restore if its primary is parseable. If its primary was
   missing/corrupt, it is retained for manual diagnostics/recovery, not offered
   as a valid primary to restore automatically.

The Windows process check uses the executable basename `TLHON.exe`
([Steam Windows depot 1766101](https://steamdb.info/depot/1766101/)).
It does not terminate processes and refuses to proceed if enumeration fails.
A renamed executable or a game launched concurrently cannot be guaranteed
to be detected; closing the game is mandatory. Automatic restore/recovery
is Windows-only; normal backup inspection is not restricted by this guard.

## Steam Cloud

Steam Cloud and the game do not honor the editor lock. The editor does not
change Steam settings. Keep the game closed, wait for synchronization to
complete, and preserve independent snapshots outside the save folder.
If Steam presents a conflict, compare the local and cloud timestamps and
the save version you intend to keep. For an intentional restore that you
have verified locally, select the local save only if it is the desired one;
do not assume the newest timestamp is the wanted progress. After syncing,
confirm the game loads the correct characters. If Steam overwrites the
restored files, close the game again, wait for sync, and restore from the
preserved snapshot. See [Steam Support: Steam Cloud](https://help.steampowered.com/en/faqs/view/68D2-35AB-09A9-7678).

## Interrupted restore

- The editor holds the same `SaveData.txt.editor.lock` as regular editor
  writes throughout restore, then writes and verifies
  `SaveData.txt.editor-restore.json` in the effective save directory.
  The journal records the target, exact old and desired file bytes and
  missing states, SHA-256 for each present file, and safety snapshot location.
  It contains private save data; keep it local. The journal is intentionally
  independent of the backup-root preference during recovery.
- Replacement data is staged using `QSaveFile` with direct-write fallback
  disabled; every changed file is staged before publishing any change.
  Each replacement is checked and the pair reread before each publish and
  after the last. The journal is removed only after both states verify.
  A successful restore leaves both the chosen and safety snapshots intact.
- Files are not a single atomic filesystem transaction. If publishing fails,
  restore attempts to roll back the exact old pair (including prior absence).
  If the app is interrupted, startup/Reload save attempts the same rollback
  before allowing edits. This is application-interruption recovery, not a
  guarantee against filesystem/hardware corruption or power-loss ordering.
- If automatic recovery cannot finish (game running, disk/permission trouble,
  damaged journal or unfamiliar external changes), edits and new captures
  remain blocked and the journal remains. Close the game, let Steam finish
  syncing, resolve storage/permission issues, then use Reload save or restart.
  The editor will not overwrite unknown external bytes during automatic
  recovery. Retrying is safe when existing files match a recorded state.
- If recovery still fails: with the editor and game closed, copy the active
  pair, journal and safety snapshot to a separate location. Compare each
  present file against the journal's before/after checksums and safety
  manifest. For a conflicting file, decide which version to retain **on
  disposable copies** first. To permit automatic rollback, reconcile the
  active files to known recorded states only after preserving the external
  version. Do not simply delete the journal to unlock editing: the pair may
  be half restored. If the journal itself is damaged, use the verified safety
  snapshot to recover the prior bytes and absence states manually; keep a
  copy of the broken journal and all current files. Only after validating the
  recovered pair and resolving the interruption should the journal be moved
  out of the save directory. Do not delete the only usable copy of a save.

## Integration for DIG-120

`RestoreService(paths, storage)` exposes `preview(id, RestorePreview&, error)`
and `restore(confirmed, safety, error)`. The preview includes verified
snapshot metadata and before/after byte maps; restore checks the preview
against a new read under the editor lock. The caller must confirm the exact
target, scope, file contents/states and Steam Cloud warning. The reusable
`Tab_Settings::restoreSnapshot(id)` implements this flow with reload and UI
refresh. `JsonParser::init()` attempts pending recovery before loading;
`SaveStore` refuses to load/edit while a journal exists. The `safety` manifest
kind is byte-integrity verified and readonly through catalog rename/delete.
Safety entries may have a missing primary; don't equate integrity to
parseability. Safety snapshots with valid primaries support normal full restore.
