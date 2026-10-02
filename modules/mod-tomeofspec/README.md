# mod-tomeofspec

The Tomes of Specialization and the specification slots they unlock.

A spec is a **loadout slot**, and the slots are **independent presets**. Slot one is the character's
own build; each Tome of Specialization (II..XX, one per additional slot, sold by the Ethereal
Bazaar's convenience vendor and the auction house) grants the swap spell of one more slot. A slot
holds **an archetype, a build and an action-bar arrangement** - nothing else - and switching to it
applies that preset; switching away records what the character held. The point is that a player can
keep a main build on slot one and experiment on slots two..twenty without wiping it.

Nothing is ever copied between slots. A slot that has never been configured is **fresh**: entering
it holds no archetype, nothing spent and an empty bar, exactly as the character started before
choosing an archetype; the player's own save is what configures it, and from then on it is restored
as it was left. Two slots may therefore hold the same archetype with different builds *and*
different bars, and the realm never mixes them.

The module owns the slot layer only. It does not validate a row, price a switch, decide an
archetype default, answer a preview, or touch the client's window state. Those are the talent
system's and the archetype system's, and they are reached through the core:

| what the slot layer needs | what it calls |
|---|---|
| apply a stored build (archetype + rows) | `ApplyAscensionTalentBuild` -> the core's upload path, unpriced |
| what the character holds right now | `GetAscensionTalentState` |
| re-state the character (0x725 then 0x0726) | `PushAscensionAdvancementState` |
| the slot number in the core's own 0x0725 push | `SetAscensionActiveSpecSlotProvider` |
| whether a swap spell is known | `Player::HasSpell` (the client's own rule) |
| the live action bar | `Player::GetActionButton` / `addActionButton` / `SendActionButtons`, rows in `character_action` |

## Why the server is the missing half

The client's spec list is already fully driven by spells, and the click already reaches the realm:

- a row is enabled while `IsSpellKnown(SPEC_SWAP_SPELLS[id])` (`CASpecListMixin:UpdateSpecButtons`),
  and the locked row's own hint is *"Unlock this specialization by purchasing a Tome of
  Specialization"*;
- the click runs `SpecializationUtil.ActivateSpecialization` -> `InternalActivateSpecialization`,
  which casts the slot's swap spell through the Extensions.dll global `CastSpecialSpell`;
- `CastSpecialSpell` sends `CMSG_CAST_SPELL` for a spell whose effect is `0xA2`
  (`SPELL_EFFECT_TALENT_SPEC_SELECT`, 162) or whose SkillLineAbility line is a profession - and all
  twenty swap spells are effect-162 spells, so the cast is exactly what the realm receives;
- that click is a **cast, not a menu action**: the client's own spell carries `CastingTimeIndex` 6
  (5000 ms) and is only started while the character is standing still, and movement or damage
  interrupts it - so the switch lands when the cast completes, and a scenario has to wait for it.

The click therefore names the slot by the **spell it casts**: `SlotForSpell` resolves the spell id
against `SPEC_SWAP_SPELLS`, and the effect's payload is only cross-checked against that answer. It
is cross-checked in both of its forms, because the payload has two in the same data: the DBC's
`EffectBasePoints_1` field is zero-based (it is what the client's table was built from), while the
value the engine hands the effect handler is one-based (`SpellEffectInfo::CalcValue` adds one when
`EffectDieSides_1` is 1, and `Player::ActivateSpec` reads exactly that: "damage is 1 or 2, spec is 0
or 1"). Reading the slot out of either value would be a silent off-by-one slot.

Nothing about the client had to change: the cast, the table and the enable/disable logic were all
already there. What was missing was the realm's side - which slots are open, which one is active,
what each one remembers - and that is this module.

## The slot space

The unlock rule is the client's, and the spellbook is its only representation:

```
slot 1 is always unlocked; slot n > 1 is unlocked while SPEC_SWAP_SPELLS[n] is known
```

`SPEC_SWAP_SPELLS` is the client's own table (`Constants.lua`), copied into the module; the twenty
spells are also checked against the server's spell data at startup by a `WorldScript` - module
scripts are registered before `sSpellMgr` has its spells, so the check belongs to world startup and
not to `Register()`. Each one must exist, be an effect-162 spell in effect slot one, carry its
zero-based slot in `EffectBasePoints_1`, and produce its one-based slot as `CalcValue()`; a data
drift is logged instead of silently trusted.
The 0x0725 push carries `{slot - 1, unlocked}` - sequentially counted, the same walk the client's
`GetNumSpecializationsUnlocked` does.

## The switch

One routine, the same for every slot; only the preset applied differs:

1. the target slot must be unlocked;
2. the slot being **left** is recorded from the realm's own state - the archetype it is on, the rows
   it holds and the bar it has arranged - read at the moment of departure, so a save, a reset or an
   archetype switch that happened while the slot was active is part of what it remembers;
3. the target slot's preset is applied. A record goes through `ApplyAscensionTalentBuild`, which is
   the core's upload path with the bill left out: validation, investment gates, the switch guard, the
   archetype's defaults and the previous archetype's rows are all exactly the ones a save goes
   through. A slot with **no record** gets the fresh state instead - no archetype, nothing spent,
   nothing copied from the slot being left;
4. the target slot's action bar is put in place: the live buttons are replaced and the change is
   sent to the client, and the same arrangement is written to the `character_action` rows the core
   reads at login, so a disconnect, a crash or a relog cannot bring back the previous slot's bar;
5. the active slot is stored and the state is pushed the way every other change is answered.

A slot whose spell is gone (a revoked Tome) is not a slot the character can be on: login and
`.tomeofspec revoke` fall back to slot one, and the fallback records the slot being left first, then
takes up slot one's own preset - nothing from the lost slot is carried into it.

A slot switch is not billed. The Tome that opened the slot is what paid for it, and the client's
spec list has no confirmation dialog to price one - so `ApplyAscensionTalentBuild` runs with
`checkBudget = false` and nothing else about the apply differs. Changing the **archetype inside a
slot** is not a slot switch: that is the client's own specialization upload, and it keeps the
core's own price and confirmation exactly as a save does (a departure from an archetype with
charged ranks bills those ranks' unlearn cost).

A slot whose spell is gone (a revoked Tome, a character whose record came from another class) is
not a slot the character can be on: the answer falls back to slot one and the fallback is written
down by the paths that can cause it (login, `.tomeofspec revoke`, a switch), never by a read.

## Storage

`player_settings`, so there is no schema change:

| key | meaning |
|---|---|
| `core.tomeofspec.slot.active` | the slot the character is on (1..20) |
| `core.tomeofspec.slot.<n>` | one record per slot the character has left configured |
| `core.tomeofspec.bar.<n>` | the action-bar arrangement that slot was left with |

A build record is this module's own and holds a build **whole and uncapped**: a mark (`'TOMS'`),
the archetype, the row count, then one `(entry, rank, locked)` triple per row. It is deliberately
not the core's loadout block (`Loadout` / `BuildLoadoutBlock`): that block's entry field is the
client loadout window's fixed 32 rows, so a longer build written into it is silently clamped - and
the core's own investment gates then unlearn further rows on the restore, because the rows those
gates counted are the ones the clamp dropped. A 52-row build came back as 26 that way; the record
shape above cannot lose a row. Records an earlier build of this module wrote in the block shape are
still read best-effort (the rows the clamp kept are the player's) and re-written whole the next
time their slot is left. A slot with no record is fresh.

A bar record is the count followed by one `(button, packed action)` pair each. The packed word is
what the core stores on an `ActionButton`, so the type travels with the button (spell, macro, item,
equipment set) and the arrangement survives a full round trip.

Only the **active** slot's bar lives in the core's own `character_action` table (rows for spec 0,
re-written on every switch); the other slots' arrangements live in the settings above until the
switch that restores them. The core keeps its own writer for those rows, and the module writes them
as unchanged, so the two cannot disagree about what the active slot's bar is.

## What is registered

- `spell_tomeofspec_slot_switch` - a SpellScript on the twenty swap spells. It prevents the default
  effect (that is the vanilla two-spec activation, and letting it run beside this one would be a
  second switch owning the same character) and routes the cast to the slot routine. The
  `spell_script_names` rows are in
  `data/sql/updates/pending_db_world/rev_20260927_20_tomeofspec_spell_scripts.sql`.
- a `PlayerScript` - login (grant the first slot's spell, normalize the active slot) and
  `OnPlayerLearnSpell` (a Tome was used: announce the slot and re-push the slot number only, so the
  client's list redraws without the entries packet resetting a pending build).
- `SetAscensionActiveSpecSlotProvider` - the core asks the slot layer what the two fields of its
  0x0725 push mean. The core keeps the single writer of that packet, and a slot character is told
  its slot and its unlocked count rather than the place of its archetype among its class's
  archetypes.
- `.tomeofspec` - `grant <1-20|all>`, `revoke <2-20>`, `select <1-20>`, `setspec <1-20> <archetype>`,
  `clearslots`, `status`. The player's own path is the cast, which no console can produce, so the
  same routine is reachable by name.

Everything above is registered whether or not `TomeOfSpec.Enable` is set - the spell names the
database binds must resolve either way - but every handler stands down on its own when the module
is off: no provider is registered, so the core's 0x0725 push keeps the archetype mapping it uses
with no slot layer, and a swap-spell cast keeps whatever behavior it has without a script.

## Not the old module

An earlier `mod-tomeofspec` (source lost; its config and a compiled copy survived in
`COA/Core/worldserver.*.bak.exe`) stored each slot's archetype and build in its own
`core.tomeofspec.slot.<n>` / `core.tomeofspec.build.<n>` settings and corrected the client's slot
number from a `StateSyncDelayMs` timer that fired after the core's push. This module keeps the
config keys and the setting names, but:

- the build is stored whole, in this module's own uncapped record - a loadout-shape record from
  this module's own earlier build is read once and rewritten whole;
- the build is applied through the core's talent path, not by re-raising rows itself;
- the slot number is authoritative in the core's push (one packet, one writer), not corrected
  afterwards by a timer - a timer that overwrites state after the fact is a symptom, and this one
  only existed because there was no way to answer the push.

No migration from the old settings is attempted: their exact layout is lost, and the authoritative
build of every character is the core's own state, which this module neither moved nor rewrote.

## Verified / not verified

Verified:

- Compiles and links into `worldserver`; the startup self-check reports the swap-spell table
  agreeing with the client's slots (`20/20 spells agree with the client's slots`).
- The client contract (spell table, effect 162, `EffectBasePoints_1` = slot, `IsSpellKnown` gating,
  the 0x0725 field meanings, `CastSpecialSpell` sending the cast) is read from the client's own Lua
  and from the reconstruction of the Extensions.dll.
- The slot space, the switch and the per-slot records under the earlier "a new slot adopts" rule:
  slot one's spell granted on login, `{slot-1, unlocked}` pushed before any Tome, a Tome opening the
  second row, the five second cast landing the switch when it completes, an archetype change inside
  a slot keeping the core's priced departure, and a charged rank bought on a slot purged by the
  native reset on that slot only (40 steps / 28 assertions, green on both the simulated clock and
  the real-clock reference; the scenario file itself still asserts the old adopt rule and is being
  rewritten to the preset rule).
- The per-slot bar path is exercised end to end in game (`core.tomeofspec.bar.<n>` written on the
  departure, the live rows in `character_action` re-written on every switch, the client sent the new
  arrangement).

Not yet verified:

- The visual result in the client (the spec list highlighting the active row, the trees showing the
  slot's build, and the bars showing the slot's arrangement) - synthetic input does not reach this
  client, so it has to be confirmed in game.
- Whether a swap-spell cast can be refused by `Spell::CheckCast` on some characters (the
  spell-script route is the same one main's Wildcard modules use on these same twenty spells).
