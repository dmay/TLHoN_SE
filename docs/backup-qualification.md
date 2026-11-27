# Backup / restore qualification (DIG-121)

## Scope and readiness

Automated qualification uses only checked-in fixtures or synthetic data in
temporary save directories. The supported Windows build is MSVC x64, Qt 6.8.3.
The real game, its installed executable and Steam Cloud are separate manual
checks. Passing editor tests does not prove that a game accepts modified saves
or that Steam will preserve a restored version. Production inventory/equipment
mutations remain gated on milestone qualification.

## Reproduce the automated checks

From a clean Windows checkout with Qt available:

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --verbose --output-on-failure
cmake --install build --config Release --prefix "$env:TEMP/tlhon-package"
windeployqt --release "$env:TEMP/tlhon-package/bin/TLHoN_SE.exe"
```

Use the exact commands in [.github/workflows/qt-build.yml](../.github/workflows/qt-build.yml)
for the clean package launch/exit smoke. That CI step uses fixture copies with
an explicit `--save-dir`, requires the effective directory in the startup log,
and compares both SHA-256 hashes after closing the editor. It does not launch
the installed game. CTest verbose output contains each test's result and
`QUALIFICATION` hash lines from the fixture lifecycle.

| Suite | Required evidence |
| --- | --- |
| savecore_tests | Default/override path resolution without filesystem writes; byte-exact single/multi-slot parsing, malformed/missing/empty saves, preservation of unrelated sections, failed reload, lock/readonly/external-write rejection |
| backupservice_tests | Unique snapshots, exact pair, label/metadata persistence, separate temp/default storage, absent/empty companion, legacy import, canceled capture, corruption/invalid manifests and unavailable storage |
| restoreservice_tests | Fixture capture → persisted edit → restore → stale editor rejection → safety undo → original pair, integrity, optional companion, missing/corrupt active, denied replacement, partial publish rollback, interruption after first/both commits, external conflict and game process guard |
| editor_tests | Real Settings/Stats entry points, all existing stat/achievement actions (save persistence only), selected-slot changes, no-op reload/mtime checks, stale controls and reload, real Backup Manager create/rename/delete/restore/cancel/recovery and stubbed failure states |

A Windows symlink test using QFile::link is skipped because it creates
shortcuts on Windows. The portable read-only test may skip when the filesystem
does not enforce the permission; the Windows restore suite separately exercises
a real sharing-denied replacement. Injected failures model application-level
interruptions, not physical disk exhaustion or a power-loss guarantee.

## Fixture hash evidence

Source baseline in the repository (SHA-256, file bytes without conversion):

| Fixture | Size | SHA-256 |
| --- | ---: | --- |
| SaveData.txt (primary, two characters) | 217097 | `c873b91f38c686cb5cf6c1c76b6375fa648ce0cc04b3c806c9188bde7f21fa69` |
| SaveData1.txt (companion, one character) | 14369 | `ee9c44ccbc0e4010db20a65dcd2864d7f34ff8911233db708a87b5290ec092ce` |

The fixture lifecycle asserts exact bytes, not just hashes, for capture,
restore, undo and final restore. The edited primary hash and all observed
before/after hashes are recorded by CTest; the companion must remain equal
throughout. The independent packaging smoke logs its own before/after hashes.

## Manual local-game qualification — not run

**Do not use a live/valuable character.** Obtain explicit authorization for a
disposable test character/save and its effective Windows save directory.
The game uses a full-save container; restoring one snapshot replaces all
characters. A `--save-dir` override isolates editor tests but does not redirect
the game's own save path.

### Preparations

- Record editor commit, installed game version and exact executable basename
  (expected `TLHON.exe`), and Steam Cloud's original per-game setting/status.
- Close game and editor; wait for Cloud synchronization. Make an independent
  external copy of the **whole** active save folder, outside both Steam and
  the editor's backup root. Verify backup contents and hashes. Do not proceed
  if a valuable existing save or cloud state could be overwritten.
- Use an explicitly approved disposable test save. For the initial test, use
  a separate approved test profile/account where practical and confirm Steam
  Cloud is disabled for that test only; do not silently change its setting.
  Keep the independent safety copy until the original save and cloud state
  are verified. Do not upload the actual save data to issue trackers.

### Test A: local, Cloud disabled

1. Start the game on the disposable save. Record test slot, current memory
   count (or a harmless persisted stat) and other slot names/count. Exit the
   game and wait for the process to stop.
2. In PowerShell, record SHA-256 of the primary and companion if present;
   explicitly record absence. Use the verified test directory, not a
   presumed path:

   ```powershell
   $test = 'C:\path\to\approved\test-save'
   foreach ($name in 'SaveData.txt','SaveData.txt.bak') {
     $p = Join-Path $test $name
     if (Test-Path -LiteralPath $p) { Get-FileHash -Algorithm SHA256 -LiteralPath $p }
     else { Write-Host "$name MISSING" }
   }
   ```

3. Launch the editor for that exact save path. Settings → Backup Manager →
   Create backup; label it `DIG-121 baseline`. Confirm both manifest hashes
   (or absence) equal the prior observation. Record the snapshot ID.
4. Select only the test slot, change the recorded stat to a nearby different
   valid value, close the editor. Hash the pair again: only the primary
   should change. Start the game, verify the edited value, and exit again.
   Game-generated rewrites during a game launch are expected; record them
   separately from editor changes.
5. With the game closed, restore the baseline through Backup Manager.
   Confirm full-save scope, correct target, safety snapshot ID and reloaded
   slot/stat. Hash the active pair **before** starting the game; exact match
   to the baseline including companion presence is required.
6. Close the editor, launch the game, verify the original stat and every
   test slot is present and loadable. Record any loading errors or differences.
   Exit the game. Do not claim game compatibility on the basis of editor
   hashes alone.

### Test B: Steam Cloud enabled

Repeat the above on the explicitly authorized disposable save/account with
Cloud enabled. Wait for synchronization before each editor operation and
record status, timestamps, selected version if a conflict is presented, and
the actual state after game relaunch. Do not deliberately create a conflict
against a valuable remote save. If no conflict is shown, record that outcome
rather than claiming conflict-resolution validation. If a conflict occurs,
choose the known intended version only after comparing it to independent
copies. Stop if Steam unexpectedly overwrites a restored save.

After both tests, close game/editor, reconcile the approved test profile and
restore any original settings/save state from the independently verified copy
as applicable. Keep that copy until verified. Record only sanitized test
results (commit, game version, settings, snapshot IDs, hashes, observed UI/game
values and failures), not save bytes or private paths.

### Result record (pending)

| Observation | Result |
| --- | --- |
| Installed game version / basename | Pending |
| Independent test backup verified | Pending |
| Cloud disabled: game accepts edit and restored original | Pending |
| Exact pair hashes before game relaunch | Pending |
| Cloud enabled: sync and post-restore game state | Pending |
| Cloud conflict encountered / disposition | Pending |
| Original test environment/settings reconciled | Pending |

Do not mark installed-game/Cloud validation complete until this record is
filled from observations on the approved local machine.
