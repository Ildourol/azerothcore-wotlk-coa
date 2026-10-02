-- mod-tomeofspec: the Tomes of Specialization on the Ethereal Bazaar's convenience vendor.
--
-- 900008 is `BAZAAR_VENDOR_CONVENIENCE` in mod-ethereal-bazaar - a virtual vendor, not a
-- creature_template row - and its stock is the rows below. Tome I does not exist: slot one is the
-- character's own build. Tomes II..XX are the nineteen slots beyond it, one item per slot, each
-- carrying its slot's swap spell (`item_template.spellid_2`, `spelltrigger_2` 6 = learn on use),
-- chained by `requiredspell` so the slots open in order. Item 106954 (Tome II) is the entry point
-- and has no prerequisite.
--
-- The ExtendedCost is the vendor's own, 2984, the same one the other Tome rows in that stock use.
DELETE FROM `npc_vendor` WHERE `entry` = 900008
    AND `item` IN (106954, 106956, 106957, 106958, 106959, 106960, 106961, 752030, 752031, 752032,
                   752033, 97400, 97401, 97402, 97403, 97404, 97405, 97406, 97407);
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `ExtendedCost`) VALUES
(900008, 27, 106954, 2984),
(900008, 28, 106956, 2984),
(900008, 29, 106957, 2984),
(900008, 30, 106958, 2984),
(900008, 31, 106959, 2984),
(900008, 32, 106960, 2984),
(900008, 33, 106961, 2984),
(900008, 34, 752030, 2984),
(900008, 35, 752031, 2984),
(900008, 36, 752032, 2984),
(900008, 37, 752033, 2984),
(900008, 38, 97400, 2984),
(900008, 39, 97401, 2984),
(900008, 40, 97402, 2984),
(900008, 41, 97403, 2984),
(900008, 42, 97404, 2984),
(900008, 43, 97405, 2984),
(900008, 44, 97406, 2984),
(900008, 45, 97407, 2984);
