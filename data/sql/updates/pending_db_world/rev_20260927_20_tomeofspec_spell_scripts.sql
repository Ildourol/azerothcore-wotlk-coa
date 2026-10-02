-- mod-tomeofspec: the twenty specialization swap spells are bound to the module's slot switch.
--
-- The client's spec list casts a slot's swap spell when its row is clicked
-- (`SpecializationUtil.ActivateSpecialization` -> `CastSpecialSpell`), and the realm answers that
-- cast with the switch. The spells are the client's own `SPEC_SWAP_SPELLS` table, in order:
-- 979993 (Spec I) through 84896 (Spec XX); every one of them is an effect-162
-- (SPELL_EFFECT_TALENT_SPEC_SELECT) spell whose EffectBasePoints_1 is its zero-based slot.
-- The click is the client's own five second cast (CastingTimeIndex 6, interruptible by movement and
-- damage), so the script runs when the cast completes; the slot is named by the spell that was cast
-- and the payload is only cross-checked against it.
--
-- `spell_ascension_spec_swap` is dropped. An earlier pass of the module bound that name to these
-- same twenty spells as well, and no code registers it: the rows only produced "no such script is
-- registered" errors at startup and would have run the switch twice for one cast.
DELETE FROM `spell_script_names` WHERE `ScriptName` IN ('spell_ascension_spec_swap', 'spell_tomeofspec_slot_switch')
    AND `spell_id` IN (979993, 979994, 979995, 979996, 979997, 979986, 979987, 979988, 84874, 84876,
                       84878, 84880, 84882, 84884, 84886, 84888, 84890, 84892, 84894, 84896);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(979993, 'spell_tomeofspec_slot_switch'),
(979994, 'spell_tomeofspec_slot_switch'),
(979995, 'spell_tomeofspec_slot_switch'),
(979996, 'spell_tomeofspec_slot_switch'),
(979997, 'spell_tomeofspec_slot_switch'),
(979986, 'spell_tomeofspec_slot_switch'),
(979987, 'spell_tomeofspec_slot_switch'),
(979988, 'spell_tomeofspec_slot_switch'),
(84874, 'spell_tomeofspec_slot_switch'),
(84876, 'spell_tomeofspec_slot_switch'),
(84878, 'spell_tomeofspec_slot_switch'),
(84880, 'spell_tomeofspec_slot_switch'),
(84882, 'spell_tomeofspec_slot_switch'),
(84884, 'spell_tomeofspec_slot_switch'),
(84886, 'spell_tomeofspec_slot_switch'),
(84888, 'spell_tomeofspec_slot_switch'),
(84890, 'spell_tomeofspec_slot_switch'),
(84892, 'spell_tomeofspec_slot_switch'),
(84894, 'spell_tomeofspec_slot_switch'),
(84896, 'spell_tomeofspec_slot_switch');
