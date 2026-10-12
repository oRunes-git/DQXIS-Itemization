# Dragon Quest XI S Itemization developer guide

Version 1.0 is the first public release, based on the verified internal development build v1.16.0. Internal policy/build identifiers are retained for compatibility and evidence. Current deployed policies and source define behavior; historical notes do not override them.

## Ownership and runtime

The launcher starts Steam XI S, loads itemization-probe.dll through load-probe.exe and verifies activation in the current process. Loading the library alone is insufficient. Native build and binding guards run before feature initialization.

ItemService separates bases from owned identities. Acquisition creates independent snapshots; crafting appends immutable revisions. Adventure logs resolve their saved revision, not another log's newest revision. Locks follow logical identities. Never distribute populated history or replace an existing player's history with fresh state.

Inventory, equipment, acquisition, battle-result, damage and UI adapters share the policies below. The Hotto bypass only handles its mandatory introduction flag; there is no separate 2D gear/combat adapter.

## Authoritative files

| Runtime file | Purpose |
|---|---|
| item-bases.json and custom-bases.json | Base snapshots, restrictions and inherited models |
| affixes.json | 91 enabled families, capacities, 33 tiers, Classes 1-15, scopes and caps |
| forge-modifiers.json | Forge performance policy required at startup |
| camp-crafting.json | 91 shard identities, tier weights and salvage policy |
| veyls.json | Nine Veyls, costs and configured rewards |
| regional-loot.json | Regional pools, world-state aliases and eligible monsters |
| rarity-progression.json | Saved story-boss credit and reward rarity |
| action-affix-tags.json and combat-target-tags.json | Actions, target families and elemental integration |
| wearer-capabilities.json | Character capability filtering |
| runtime-config.json | Feature switches, build identity and diagnostics |

Saved affix bounds and rolls are frozen on load. Balance edits affect fresh acquisition and explicit crafting. Flat weapon effectiveness applies once; quality must not compound. Attack additions stay with their source weapon. Spell additions sum eligible wearer equipment and follow each actual hit's coefficient.

## Transactions and UI

Currency debit, item replacement, shard delivery and balance verification form one operation. Refused delivery must not consume gear without rewards. Salvage rechecks location, ownership, equipped/protected state and locks before removal, skipping newly locked entries.

Crafting follows the current party. Focus changes must redraw even when neighboring items have identical names/icons/stats. Empty salvage rows are absent from render and selectable data. Back must restore the forge's visibility and native focus.

## Builds and evidence

Microsoft x64 C++ tools compile C++17 with the static runtime. build-probe.ps1 builds the runtime and loader. Other build scripts cover shared affixes/combat, tier scaling, Kingmaker, Veyls, camp crafting, spells and UI. Retain JSON and MinHook licenses.

The current release proof is outputs/DQXIS-Independent-Crafting-v1.16.0/release-manifest.json and its named validation reports. The package builder verifies those proofs and deployed hashes. Its three patches are ClassLabel_1, Crafting_3 and CraftingUI_1. Obsolete Veyls_2 and Test_1 patches are not shipped.

tools/refresh_documentation.py generates current Markdown. tools/package_desktop_mod.py creates the standalone folder and ZIP. tools/test_desktop_package.py checks Windows PowerShell install/update/import, corruption refusal and rollback in disposable folders. No fixtures or test saves are distributed.

Never replace a loaded module or launch the game without the user's instruction. Portable tests do not prove every native branch. User checks cover menus, regional drops, locks, shard/salvage delivery, repeated crafting, quality, Kingmaker and Hotto. Veronica's Bang exercised added spell damage; that smoke test does not exhaust all spells.

## Retention and deferred scope

Keep source, tests, original art, current proof/release/runtime and extraction evidence in the private workspace. Recovery retention keeps the vanilla baseline, latest pre-update backup and paired current v1.16 snapshot. The cleanup report lists removed obsolete releases and backups.

Ship only approved mod payloads, current guides and licenses. Exclude saves, populated history, debug traces, test requests and private recovery data. Kingmaker's natural late-game award is a future authoring decision. Full 2D support remains excluded.
