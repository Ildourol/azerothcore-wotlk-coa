# CoA Camping

An optional AzerothCore module implementing the first camping slice from the Forever handoff captured on
4 October 2026, build `1.60.1.70009`. Camping is disabled by default. The module uses existing core hooks;
its only core-side addition is a shared-gameobject interaction fixture in the gameplay test harness.

## Build and enable

The normal `MODULES=static` build discovers this directory and `Addmod_coa_campingScripts`. Reconfigure CMake
after adding it. Set `-DMODULE_MOD-COA-CAMPING=disabled` to exclude the entire module, including its unit tests.

The pending world migration `data/sql/updates/pending_db_world/rev_20261005_01_coa_camping.sql` adds two inert
templates. Let the normal updater apply it to the chosen deployment. It preserves any template already using
either reserved entry; enabled startup then rejects an incompatible mapping. No existing fire, spell or Ranger
template is changed.

Copy `conf/mod-coa-camping.conf.dist` to the server's module configuration directory without `.dist`, set
`CoACamping.Enable=1`, and restart. Enabled startup rejects missing or incompatible spells, templates,
displays, materials, skills and invalid configuration. Settings are immutable until the next restart.

## Playing

1. Cast Basic Campfire (818) on allowed outdoor terrain while alive and out of combat. The default map
   allowlist is 0, 1, 530 and 571. Flight, transport, vehicles and underwater placement are excluded.
2. Click the nearby **Campsite Supplies** alchemy set. Any accessible player with Herbalism 20 can contribute
   one **Incense Candle** using one Peacebloom and one Silverleaf. One candle per camp is supported initially.
3. Sit within 20 yards of the fire for 60 uninterrupted seconds. The candle grants **+2 Intellect for one hour**,
   presented as stock Arcane Intellect. Chair sitting counts. Standing idle does not.

Movement, standing, combat, death, logout, teleport, phase changes or removal of the camp reset unfinished rest.
Eligibility is checked every player update; camp selection and completion are scanned once per second.
The rest clock starts once a candle is present. A completed rest grants once; stand and sit again to begin
another rest. A completed rest may refresh a camping-owned reward, including one retained after relog.

Contribution requests are committed on the map update lane and rechecked after opening gossip. Skill,
carried materials, range, line of sight, phase, capacity, expiry and the shared character cooldown must still
be valid. Rejected requests or failed prop creation consume nothing. Successful contribution saves inventory
and cooldown in one character-database transaction. The cooldown defaults to one hour across every camp and
survives relog/restart. The `core.coa.camping` character setting is mandatory gameplay state even when optional
player preferences are disabled; no new character table is needed.

Managed fires last fifteen minutes by default, together with their supplies, reward helper, linked trap and
attachments. Static fires, differently summoned fires and Ranger spell 807955 never enter the registry.
Only one camp per owner is allowed, with 40-yard spacing in the same phase. Camps are removed when the owner
logs out, leaves the map or changes phase, when the fire/controller disappears, on expiry or map unload.
Owner death keeps the camp. Camps do not survive restart. Paid materials are not refunded on cleanup.
Completed rewards retain their own one-hour lifetime after the camp is gone.

## Mappings and fidelity

`src/CoACampingMapping.h` is the mapping source. The core checks its entries against effective startup data.

| Role | Mapping |
| --- | --- |
| Fire | Spell 818, object 29784, display 192 |
| Supplies | New object 9500200, stock alchemy-set display 345, gossip goober |
| Candle | New object 9500201, stock candle display 100 |
| Reward | Spell 1459, effect 0 only, 2 Intellect, invisible stock World Trigger caster |
| Contribution | Herbalism 182, base rank 20; items 2447 and 765, one each |

The profession gate and reagents match the catalog's placement-item rank and crafting materials. The first
reward deliberately uses the lowest stock Arcane Intellect amount at every level; it does not implement
Forever's level bands. Only the Intellect effect is applied, even when CoA's effective Arcane Intellect has
additional effects. Existing Arcane/Dalaran Intellect and Brilliance ranks are preserved, including caster
ownership and remaining duration. A subsequent class cast replaces only the camping-owned proxy before the
native class effect applies, so the strongest class effect can win through normal core stacking.

The allowlists, radii, spacing, lifecycle and seated-only policy are implementation choices from the handoff,
not observed Forever server behavior. The capacity setting excludes the fire, supplies and helper; because
the first slice has one feature family, at most one slot is currently used. Other profession families,
upgrades, tents, recipe unlocks, vendor/repair services, seed nodes and legacy perks remain unimplemented:
their acquisition, economy or exact behavior was unresolved in the supplied evidence.

The display and spell IDs are present in the locally extracted CoA DBCs. An unmodified build-12340 client
and the broader fork's stock compatibility still require separate in-game acceptance. The module adds no
client patch and does not replace any existing DBCs. Native scenarios exercise server behavior only.

## Verification

Reconfigure the build, then use the repository's verification command:

Windows runs require the linked MySQL and OpenSSL DLLs and OpenSSL's `legacy.dll` beside the build
executables. Run the two configuration profiles sequentially because the default reusable world cache is
leased by one verification batch at a time.

```sh
python -B tools/verify_all.py --base HEAD --stages source,build,unit --query camping
python -B tools/verify_all.py --stages gameplay --query camping --settings path/to/camping-verify.json
```

Gameplay settings must select module configurations with `CoACamping.Enable=1`. The scenarios keep native
spell costs/cooldowns and gossip handlers, and cover shared contribution, revalidation, persistent cooldown,
rest interruption, class-buff ownership in both orders, fire identification and cleanup. Unit tests cover
elapsed-time rest transitions and contribution gates. Deploying the module and checking client rendering
are separate from this source change.

The deadline check is separate because its two-minute lifetime differs from the default fifteen-minute
profile. Use another module configuration directory with `CoACamping.LifetimeSeconds=120` and select that
directory through `path/to/camping-expiry-verify.json`:

```sh
python -B tools/verify_all.py --stages gameplay --settings path/to/camping-expiry-verify.json \
  --scenario coa-camping-contribution-revalidation modules/mod-coa-camping/tests/expiry/coa-camping-expiry.json
python -B tools/verify_all.py --stages harness --harness test_camping_migration --settings path/to/camping-verify.json
```
