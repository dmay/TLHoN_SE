# Snapshot storage (DIG-118)

Settings → Backup / recovery has temporary actions to create a snapshot, choose
its storage folder, and import the older editor backup. Close the game first;
Steam Cloud and the game do not honor editor locks. DIG-119 adds restore and
DIG-120 replaces the temporary actions with the full Backup Manager.

- Default root: `%LOCALAPPDATA%/TLHoN_SE/Backups` (Qt generic local data fallback).
- Preference: `QSettings("DeathWhisper", "TLHoN_SE")`, key
  `Backup/storageDirectory`. Choosing another root switches catalogs; it does
  not move existing snapshots. Storage must be separate from the current and
  default game save folders.
- Each snapshot has a UTC millisecond timestamp + UUID directory, manifest and
  byte-exact copies. Files are staged on the backup volume, verified, and then
  published by directory rename. Abandoned `.staging-*` directories are not
  completed catalog entries.
- Manifest v1: `id` (directory basename), `version`, UTC `createdAt`, `kind`
  (`normal` or `legacy`), `label`, `sourceDirectory`, `sourceProfile`, and
  ordered `files`. Each record has its exact original filename and state
  (`present`, `missing`, or `notCaptured`); present entries also include decimal
  string size in bytes and lowercase SHA-256 hex.
- Normal snapshots require `SaveData.txt`; the companion `SaveData.txt.bak` is
  recorded as present or missing. A legacy snapshot captures only
  `SaveData.txt_bckp`; the companion is explicitly not captured, not missing.
  Import leaves the original legacy file untouched.
- A successful capture or `inspect` verifies manifest and file integrity, not
  the game's save format. Malformed save bytes can be backed up for recovery or
  diagnostics; a restore must separately parse/validate the candidate.
- `list` returns entries newest-first, including managed but invalid entries
  with diagnostics. `readVerified` returns exact verified content by fixed
  filename (no bytes on failure). `renameLabel` atomically replaces only the
  manifest of a valid entry. `deleteSnapshot` requires explicit confirmation
  for a basename, rejects traversal, links and unexpected entries, and never
  recursively deletes arbitrary files. Failed cleanup is reported with a
  separate unpublished `.deleted-*` directory.
- Capture uses the same `.editor.lock` as SaveStore; it rereads the full source
  set before publication, including optional-file presence. This detects
  observable edits during capture, not all conceivable uncoordinated race
  patterns. The game should be closed. The callback permits cancellation
  before initial capture and before the final check, without publishing a
  partial entry.

## Restore integration

Use `BackupService(paths, BackupService::configuredStorageDirectory(settings))`
and `readVerified(id, snapshot, bytes, error)`. For normal snapshots use
`bytes["SaveData.txt"]` and the explicit companion state. For legacy use
`bytes["SaveData.txt_bckp"]` as a primary-only source; do not infer anything
about the game companion. The service does not write to game saves. DIG-119
must extend capture for missing-primary safety snapshots and coordinate the
editor lock across the restore transaction (normal capture currently acquires
it itself); do not call normal capture while already holding that lock.
