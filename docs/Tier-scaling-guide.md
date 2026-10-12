# Affix scaling: Class 1–15 / T1–T33

Classes 1–12 unlock two tiers each: their ceiling is `Class × 2`.
Class 13 unlocks T27, Class 14 T30, and Class 15 T33.
Veyl of the Kingmaker raises Class by exactly one, costs one Veyl, and stops
at Class 15. It keeps existing affixes, rolls, quality, base stats and locks.

All 91 affix families retain their former global minimum at T1 and their
former global maximum at T24. The numerical range is divided into 24 equal
bands; T25–T33 continue the same linear slope. Fractional band endpoints
round up to integers. Adjacent bands share an endpoint. Small numerical
ranges can produce fixed-value tiers or repeat a value across adjacent
tiers. Existing two-handed flat-affix multipliers still apply.

For example, unscaled added elemental damage previously spanned 1–75.
Its new T24 band is 72–75. T27 is 82–85, T30 is 91–94 and T33 is 100–103.
Heavy wands still double those flat amounts.

Existing gear retains its frozen historical tier label, range and roll.
Loading the update, using Kingmaker and quality reforging do not rewrite
those snapshots. Attuning an old modifier rerolls its original saved range.
An explicit shard/Empowering upgrade moves a legacy modifier into the first
eligible new band whose minimum reaches its old range maximum and whose
maximum exceeds it. Its preview shows the actual destination tier. If the
current Class cannot reach that band, the upgrade remains unavailable.
Freshly crafted modifiers then upgrade one new tier at a time.

New loot and full rerolls use the new scale. Item type, rarity capacity,
affix eligibility, shard IDs/icons, native reward rates and aggregate combat
caps remain unchanged. Salvage still rolls 30% once per affix and 10% once
per tier above T1, now supporting up to 33 independent rolls at T33.

