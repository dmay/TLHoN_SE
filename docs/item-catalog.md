# Item definition catalog (DIG-117)

The catalog is a **read-only, version-scoped definition index**, not an inventory
editor or a list of all obtainable items. It does not modify, load automatically,
or launch the game or the save editor. No game assets, installed catalog, or
new player saves are distributed with the editor.

## Build and inspect

With Qt 6 (or Qt 5.15), CMake 3.16+, and a C++17 compiler configured for Qt:

```powershell
cmake -S catalog -B build-catalog -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build build-catalog --config Release
ctest --test-dir build-catalog -C Release --output-on-failure
```

The extractor is a standalone build while DIG-115 changes the shared
editor CMake project in parallel. It provides the `tlhon_item_catalog`
library target and `tlhon-catalog` console executable. Consumers can add
`add_subdirectory(catalog)` and link `tlhon_item_catalog` in the top-level
editor CMake project when they integrate the service. Dedicated Windows CI
exercises the independent target. For a single-config generator the tool
is in the build directory instead of `Release/`.

From the repository root, on Windows (substitute your actual game directory;
write the report **outside the game installation**):

```powershell
.\build-catalog\Release\tlhon-catalog.exe --game-dir "D:/SteamLibrary/steamapps/common/The Last Hero of Nostalgaia" --save-fixture Resources/Tests/SaveData.txt --save-fixture Resources/Tests/SaveData1.txt --legacy-armor-source tab_stats.cpp > catalog-report.json
```

Omit `--game-dir` to discover the installed Steam app 1766100 via Windows'
Steam registry/default roots and `steamapps/libraryfolders.vdf` (including
secondary libraries). Supply `--steam-root` to override Steam discovery.
The matching `appmanifest_1766100.acf` supplies the installed Steam build ID
when available. An explicit directory looks for
`TLHON_Data/StreamingAssets/aa/catalog.json`; a copied metadata-only file can
be supplied via `--catalog path/to/catalog.json` instead. Missing/unrecognized
catalogs or ambiguous installs fail with a diagnostic and nonzero exit.
The tool writes its JSON report only to stdout and does not access the
normal active-save directory.

`--save-fixture` is optional and repeatable: provide repository fixtures or
**disposable copies** only. It compares unique, exact definition GUIDs from
both inventory arrays across all fixture characters, including mismatched
array/category classifications. Armor slot evidence is obtained by resolving
the four observed equipped armor fields against unique inventory instance
IDs, not by treating IDs as slot-name suffixes. The report includes observed
slots even for missing catalog GUIDs, and reports disagreements with reviewed
slot metadata without changing it. A save observation is **not** proof of
obtainability, DLC entitlement, or that a newly generated item will load.

`--legacy-armor-source` optionally compares the historical
`const QList<QString> armor` initializer in `tab_stats.cpp`. If the
initializer has since been removed, use the version at commit `0b07458`
as the comparison input. Matches in both comparisons are exact and not
case-normalized; `GUID` is not `instanceIDOnCreation`.

## Schema version 1

The service interface is in [catalog/itemcatalog.h](../catalog/itemcatalog.h):
`locate`, `load`, `Catalog::find`, `Catalog::bulkEligible`.
The caller supplies a source and an optional reviewed manifest. Errors
return false, a diagnostic, and an empty output (never a partially loaded
catalog). The editor never needs to invoke the CLI or parse its report:
both use the same native Qt service. Inventory and equipment mutation, DLC
ownership detection, and UI selection are separate work.

| Report field | Meaning |
| --- | --- |
| `schemaVersion` | Report format, currently 1 |
| `steamAppId`, `steamBuildId` | 1766100; matched manifest build ID or empty |
| `catalogSha256` | SHA-256 of exact catalog file bytes; authoritative version binding |
| `locatorId`, `addressablesBuildHash` | Metadata from `m_LocatorId`, `m_BuildResultHash` if present; otherwise empty |
| `internalIdCount`, `ignoredPathCount`, `repeatedPathCount`, `uniqueGuidCount` | Input and extraction/dedup counts |
| `categoryCounts`, `availabilityCounts`, `reasonCounts` | Counts over distinct GUIDs, including excluded candidates |
| `entries` | Stable GUID-ordered entries (fields below) |

| Entry field | Meaning |
| --- | --- |
| `guid` | Exact case-sensitive stem of an item .asset path; save definition GUID |
| `sourcePaths` | Distinct full Addressables internal IDs; multiple paths for one GUID are quarantined |
| `category` | armor, weapon, shield, other |
| `type`, `typeVerified` | Path-derived category/subfolder hint or reviewed type; verified flag is true only when explicit type metadata is supplied |
| `slot` | Empty until explicitly reviewed; armor slots are helm, torso, arms, legs |
| `equipmentSlots` | Explicit reviewed slot eligibility; empty if absent (see equipment service) |
| `displayName`, `displayNameVerified` | Verified name from review if available (flag true); otherwise exact GUID |
| `dlc` | unknown, dlc-path (path hint), base, dlc (requires entitlement) |
| `availability` | unresolved, excluded, validated |
| `reason`, `evidence` | Classification reason and explicit review evidence, if available |
| `bulkEligibleWithoutDlc` | True only for reviewed base-game armor, weapon or shield |

Only paths rooted exactly at `Assets/Design/GameplayData/Items/` with a
case-sensitive `.asset` extension are considered. Materials, textures,
prefabs and bundle addresses with other extensions are ignored. Candidate
paths classified as armor include `Armour` (including the observed nested
`Armour/DLCArmours` and `Armour/StartingArmors` layouts), `DLCArmours`, and
`StartingArmors`; `Weapons` and its subfolders distinguish weapon types;
`Shields` (including under `Weapons`) is separate. Unknown top-level
categories remain indexed as other, with no bulk eligibility.

A path is only a candidate; the extracted metadata does not prove the
referenced asset's runtime type. `BaseTypes` and known non-definition
folders (`Textures`, `Materials`, `Prefabs`, `Icons`) are conservatively
excluded, conflicting paths with the same GUID are quarantined, and
placeholder/prototype name hints are flagged unresolved. DLC path hints
match exact folder components (including nested `DLCArmours`) and reject
base-game allow-list entries; they are not ownership checks. The existence
of a .asset, its filename, inventory observations or the historical
hardcoded list cannot automatically grant it.

## Reviewed allow-list

Inspect the report and independently verify each eligible item against the
matching build (definition/type, player obtainability, DLC requirements,
and for armor, its actual slot). Verify display names against game metadata,
or omit them. For non-obtainable/placeholders/prototypes, record an explicit
exclusion and evidence; unresolved entries should stay unreviewed.
Conflicting GUIDs and structural exclusions cannot be allowed without a
new extractor/classification decision. Do not automatically generate an
allow-list from the inspection report.

The reviewed manifest is a local trusted input, not a signed publisher
assertion. Its entries must bind to the exact catalog byte hash and one exact
GUID/source path. The following is an **illustrative format**, not a real
validation or an allow-list to use with the game:

```json
{
  "schemaVersion": 1,
  "catalogSha256": "<SHA-256 from the local report>",
  "entries": [
    {
      "guid": "DemoArmor",
      "sourcePath": "Assets/Design/GameplayData/Items/Armour/DemoArmor.asset",
      "status": "allow",
      "category": "armor",
      "slot": "helm",
      "dlc": "base",
      "evidence": "<verified build-specific obtainability, slot and DLC evidence>"
    },
    {
      "guid": "Placeholder",
      "sourcePath": "Assets/Design/GameplayData/Items/Armour/Placeholder.asset",
      "status": "exclude",
      "reason": "placeholder",
      "evidence": "<verified reason why it is not obtainable>"
    }
  ]
}
```

Exclusions require a reason code: non-obtainable, placeholder, prototype,
dlc-unverified, or other (plus specific evidence).
For weapons and shields omit `slot`; optional `type` and `displayName`
are reviewed metadata. For DLC use `"dlc": "dlc"`; base cannot be asserted
for an identified DLC path. Unknown entitlement means no allow entry.
Pass `--review path/to/review.json` for a reviewed report. Stale hashes,
unknown/ambiguous allow entries, duplicate reviews, invalid categories,
missing evidence or invalid slots cause the entire load to fail. Without
a review, bulk eligibility is empty. Even a reviewed DLC entry requires
an explicit entitlement decision by the caller (default false).
A validated entry is not by itself authorization to write a save or an
assertion that instance factory defaults are correct.

DIG-125 extends review with optional explicitly verified hand compatibility
and an equipment-only review status, which does **not** enable item grants.
See [equipment service](equipment-service.md) for exact slot names, review
rules, eligibility and fixture limitations.

## Validation evidence and outstanding qualification

The committed synthetic catalog intentionally uses some identifiers already
present in repository saves but **invented test paths**; it is not derived
from the installed game's catalog. It has 22 internal IDs, six ignored
paths, one repeated path, 14 distinct GUIDs (seven armor, three weapon,
two shield, two other). Eleven are unresolved and three excluded;
no review provides zero bulk-eligible items. Automated tests cover
exact case, conflicting GUIDs, paths, slot evidence (including nonstandard
TopHat reviewed slot), explicit review, DLC gating, malformed/missing
inputs, stale hash, Steam discovery and byte preservation.

At original default-branch commit `0b07458`, the checked-in fixtures
contain nine inventory observations across one character (SaveData1.txt)
and 351 across two characters (SaveData.txt, 241 unique GUIDs). Exact
synthetic-path matching finds 4/9 and 5/241 unique fixture GUIDs, respectively;
these deliberately partial matches are not a claim about the installed
catalog. The historical armor list actually contains 66 distinct strings
(one matches the synthetic armor subset). The four starter armor slots
resolve to Starting_Knight definitions in SaveData1.txt; the equipped
slots in the second SaveData.txt character resolve to MasterChefArmor
definitions through their instance IDs. This is fixture evidence, not
live-game behavior.

The installed-catalog qualification findings are recorded in DIG-117.
A separate read-only inspection on the authorized Windows machine
independently reproduced extraction and fixture matching against the real
catalog, and identified the nested DLC armor path corrected here. That
inspection was a Python reproduction of the classifier/comparison logic,
not a run of the native Qt tool. Use the command above to produce a native
report for the installed version and review candidate exclusions, DLC
ownership/obtainability, slot/type and names against that version. Any
unresolved records stay ineligible.
A live verified allow-list and disposable-profile round-trip have not been
established by synthetic tests; do not enable bulk add on this basis.
