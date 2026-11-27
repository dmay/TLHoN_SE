# TLHoN Save Editor

## Why Beta?
<p align="justify">
  I decided to release a beta version of the application to check its usefulness and relevance among users. The main features, which I believe are the most interesting, have already been implemented. Your feedback will help me understand whether it is worth continuing the development of the project and in what direction to move forward.
</p>

## Opportunities
### What is ready
Not all features are completed at the moment, but most of them are. <br>
You will be able to:
* Increase/decrease your hero's memory count to "impossible" values
* Increase/decrease the hero's icons count
* Set any stat value
* Assist in completing achievements
* Add all spells and runes to your hero
* Reset all quests (except the main one) without the need to start NG+ (not tested)
* Reset all mentions of assistance in killing bosses (not tested)
* Remember or unburden all items <b>*</b>

<b>*</b> Actually, your items will not change, but an achievement will be completed.

### What is in progress
* Adding all armor and weapons (for this, I need to find them or get a save file)
* Editing the stats of individual inventory items
* Editing equipped items

Development: [read-only item catalog inspection and validation](docs/item-catalog.md).

### What may be added
* A challenges tab with the ability to check their completion

## Interface
<p align="justify">
  At first glance, the interface may seem unappealing (and to some extent, it is), but this is done to maintain visual consistency with the game. Fields marked with [NULL] are not errors; they are intentionally left that way because I couldn't find the data for those parameters in the file. (If anyone manages to find them, please let me know.)
</p>

### Equipment and Bag
<p align="justify">
  In the bag section, you will be able to add any item available in the game to your inventory and edit their parameters, such as setting astronomical damage for the starting dagger. The equipment tab will allow you to change your equipment, just like in the game. (However, it will likely be replaced with something else, such as challenges.)
</p>
Currently, these sections are empty and have temporary placeholders.

| ![1_en](https://github.com/user-attachments/assets/fb5f73ab-0f5b-484d-9b31-63d8fe20413b) | ![2_en](https://github.com/user-attachments/assets/a9779786-17b4-49ab-af0c-33aef756078e) |
| --- | --- |

### Stats
In this section, you can change any characteristic to any value, even exceeding the game's limit of 99. The editable fields are those that change color to pale yellow.

| Expluatation example | Ingame interface |
| --- | --- |
| ![stats_en](https://github.com/user-attachments/assets/78fa00d8-cc2f-43ed-b844-551c04aca78e) | ![ref_en](https://github.com/user-attachments/assets/2d27c369-638b-428e-a3c6-f3a6715c5b15) |

| Editing result |
| --- |
| ![stats_res](https://github.com/user-attachments/assets/bbd2b050-6aaa-4922-97b0-a83c4011089a)  |

| Hidden panel with functionality | Most options have a tooltip <br> that briefly describes what <br> the option does |
| --- | --- |
| ![cm](https://github.com/user-attachments/assets/540159ac-a1b0-442e-8ba8-65e70805c91a) | ![hover_en](https://github.com/user-attachments/assets/9c05a672-40df-401c-8c09-24d04f48c2ae) |

| Settings | Details |
| --- | --- |
| Everything is standard here: language change, character selection, and editing the number of icons, and something interesting. | A small greeting, links to my contacts, and the ability to report any found errors. |
| ![4_en](https://github.com/user-attachments/assets/bb69129b-3685-4720-87e3-3d337b14eecb) | ![5_en](https://github.com/user-attachments/assets/a16e6116-9647-4ab7-84b0-ba86f9e4c414) |


## Development and save safety

Build with Qt 6 (Windows CI uses Qt 6.8.3, MSVC x64), CMake 3.16+ and a C++17 compiler:

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Install the MSVC Qt 6 kit and set `CMAKE_PREFIX_PATH` to its kit directory if
CMake does not find Qt. Tests use disposable temporary directories and existing
repository fixtures, never the active game saves.

The editor defaults to `%USERPROFILE%\AppData\LocalLow\OverTheMoon\TLHON`.
To test with a copied save instead, launch with an explicit directory:

```powershell
.\build\Release\TLHoN_SE.exe --save-dir "C:\temp\tlhon-test"
```

Close the game before editing. Each edit compares the active file to the
loaded bytes before staging and again immediately before atomically replacing
it; a lock coordinates editor instances. This cannot prevent a game or another
program that ignores the lock from writing in the final check/replace window.
Ordinary edits leave the game backup (`SaveData.txt.bak`) and any existing
legacy editor backup (`SaveData.txt_bckp`) untouched. In **Settings → Backup
Manager**, create and name complete save snapshots, inspect the catalog, and
restore selected snapshots with an automatic pre-restore safety copy. See
[Backup Manager](docs/backup-manager.md) and [restore and recovery](docs/restore-recovery.md).

### Save access API

- `SavePaths` resolves the directory and each distinct filename.
- `SaveDocument::parse` / `serialize` work on bytes without filesystem access.
  The settings object, three required objects per character and envelope
  delimiters are validated. The miscellaneous section must be present;
  object-shaped chunks are checked for valid JSON, and opaque chunks are
  preserved without assuming a schema. Unchanged objects, delimiters and
  miscellaneous sections are returned byte-for-byte.
- `SaveStore` loads the envelope and owns the last known-good state.
  `mutateCharacter(index, callback, error)` edits a detached copy and only
  publishes it in memory after successful persistence. A failed reload
  prevents mutation of retained stale state.
- App widgets use the `JsonParser` facade: read-only `characters()`, `paths()`,
  `canEdit()`, `init(error)` to reload, and `mutateCharacter` for writes.
  After successful recovery, Settings can call `loadGameSaves()` and emit
  `saveRestored` so other tabs refresh.
