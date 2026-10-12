# Dragon Quest XI S Itemization player guide

Version 1.0. This guide covers installation, crafting and item progression in the supported 3D adventure. Equipment has independent affixes and persistent identities. Use the mod launcher whenever you load a modded adventure log.

## Installation and saves

Extract the complete release folder, close XI S and double-click Install.cmd. The installer finds the supported Steam XI S build 6291077 or asks you to select its executable. Start through the DQXI S - Itemization Desktop shortcut and wait for Itemization is ready before loading a log. Plain Steam launch does not initialize the mod.

The installer preserves existing item history and never edits adventure logs. It can import history through a previous Itemization shortcut. Back up adventure logs together with state/item-history.json from the runtime actually in use. Generated equipment depends on both; an adventure log alone is not a complete modded backup. Do not delete history or load a modded log with the mod removed.

The standalone installation uses %LOCALAPPDATA%/DQXI-Itemization/Runtime. A development installation may still use its original folder; the shortcut's working folder identifies it. The distributed package contains empty initial history, without personal saves, existing gear, test grants or recovery backups.

## Equipment and rarity

Copies of the same base remain separate items. Rarity controls explicit modifier capacity; implicit base effects use no slots.

| Rarity | Icon background | Maximum modifiers | Maximum of either kind |
|---|---|---:|---:|
| Normal | Grey | 1 | 1 |
| Magic | Blue | 3 | 2 |
| Rare | Yellow | 5 | 3 |

Item type, groups and weapon damage limits also restrict eligibility. Empty slots can remain after crafting. Shops and fresh forging create Normal gear. Saved story-boss credit controls supported reward rarity.

Ordinary regional equipment drops use the location and world-state version where the battle was won. Defeated monster records are checked against that pool. The configured roll is 12% per eligible defeated monster, with at most three bonus pieces per battle. Veyl rolls are independent.

## Crafting

Open Crafting from the main menu or campsite forge. Each currently available character has an equipped-items column; the shared Equipment Bag has its own column. Select gear, then switch between Veyls, Prefix shards and Suffix shards. Eligible shards appear first. Ineligible shards appear below a separator and cannot be applied.

Apply Craft plays a short completion sound and stays on the action for repeated crafts while usable. Exhausting the currency or reaching a limit returns to the list. Changed and added affixes remain highlighted in stronger yellow for inspection; removed affixes are not listed.

### Veyls

| Currency | Effect | Cost |
|---|---|---|
| Veyl of Renewing | Recreates all modifiers on Normal or Magic equipment. | 1 |
| Veyl of Enchanting | Raises Normal equipment to Magic, keeping its modifiers and adding one. | 1 |
| Veyl of Growing | Adds one random eligible modifier to an empty slot. | 1; 2 on Rare |
| Veyl of Ascending | Raises Magic equipment to Rare, keeping its modifiers and adding one. | 1 |
| Veyl of Reforming | Recreates all five modifiers on Rare equipment. | 1 |
| Veyl of Erasing | Removes one random modifier. Rarity is unchanged. | 1 |
| Veyl of Attuning | Rerolls the selected modifier value within its tier. It can improve or worsen. | 1 |
| Veyl of Empowering | Raises the selected modifier by one tier and rolls its value. Class limits apply. | Target T2-T12: 1; T13-T20: 2; T21-T33: 3 |
| Veyl of the Kingmaker | Raises item Class by 1, up to Class 15. Keeps all modifiers, rolls and quality unchanged. | 1 |

Veyls do not consume Perfectionist's Pearls. Attuning can improve or worsen a roll. Kingmaker appears after the current log has acquired it once and stays visible at zero balance. Its late-game award location remains undecided; the release does not grant it naturally or include test stock.

### Shards

There are 91 shard families, one per modifier family. One shard fills a legal empty slot with its family, rolling a tier and value within the Class cap. If that family is already present, one matching shard raises it one tier and rolls within the new band, including on an otherwise full item. Shards cannot replace other families or raise rarity. Other affixes, quality and locks remain unchanged.

## Salvage and locks

Open Salvage equipment at the campsite forge. Only generated gear in the shared Equipment Bag can be salvaged. Move character-bag gear there first. Equipped, locked and protected/story gear is excluded. Empty entries are absent and cannot be selected. Select up to 100 items, review their chances and confirm permanent removal.

Each explicit affix receives one independent 30% shard roll and one independent 10% roll for every tier above T1. Bonus rolls run even if the first roll fails. Each success gives one matching shard. Implicits and quality add no rolls; zero shards is possible.

| Affix tier | Rolls | Maximum shards | Average shards |
|---|---|---:|---:|
| T1 | One at 30% | 1 | 0.3 |
| T5 | One at 30% and four at 10% | 5 | 0.7 |
| T24 | One at 30% and 23 at 10% | 24 | 2.6 |
| T33 | One at 30% and 32 at 10% | 33 | 3.5 |

Lock through Items > Equipment Bag > select gear > Do What? > Lock. The row becomes Unlock. Each logical item has its own lock, retained across revisions and older logs. Locked items are absent from Salvaging. The final transaction rechecks locks and skips stale protected entries without consuming them or awarding shards. Equipping, transferring, crafting and reforging remain available.

## Forging and quality

The campsite forge offers Forge equipment, Reforge Quality, Crafting and Salvage equipment. Forge equipment retains recipe materials and the hammering minigame. Reforge Quality uses the unenhanced base's original pearl cost and the same minigame. Select Reforge to start; cancelling beforehand spends nothing.

Quality equals the forging score multiplied by 0.25, rounded up, capped at 25%. Score 81 gives 21%; score 100 gives 25%. Reforging overwrites quality even when worse. It retains the base, rarity, Class, affixes and original rolls, and lock.

Quality multiplies implicit and affix contributions by 1 + quality/100. Whole-number stat contributions round down separately; percentage contributions round up to the next whole percentage point. Attempts start from original saved contributions, so quality cannot compound. Combat caps still apply.

## Class and tiers

Classes 1-12 unlock two tiers each: Class 1 reaches T2, Class 2 T4 and Class 12 T24. Class 13 reaches T27, Class 14 T30 and Class 15 T33. Kingmaker raises Class by one up to 15. Current affixes, tier labels, ranges, rolls, rarity and quality do not change; the higher cap applies to future crafting.

The former lowest and highest values span T1-T24; higher tiers continue linearly. Fractional band endpoints round up. Saved gear retains historical ranges and rolls. Attuning uses those saved ranges. An explicit upgrade of old gear enters the first eligible new band safely exceeding its old range, potentially skipping multiple labels. Tier-scaling-guide.md details that migration rule.

## Added elemental spell damage

Seven prefixes add Fire, Ice, Wind, Earth, Lightning, Light or Dark spell damage using the Spell Damage gear pool and flat elemental ranges. Each actual offensive spell hit receives the addition with that hit's multipliers. Frizz gains one hit's addition; multihit spells receive it on each actual hit. Components retain their own elemental resistance, the native critical result and applicable spell bonuses. Healing, buffs, status-only and MP-only effects gain no damage.

## Supported mode

Stay in 3D with generated gear. Optional 2D mode and Tickington do not support custom item identities and can lose equipment. The mandatory Hotto introduction is bypassed, including on an already completed Hotto save. It does not finish or reward pastword adventures, make optional 2D entry safe or repair damaged 2D saves. Full 2D support is outside scope.
