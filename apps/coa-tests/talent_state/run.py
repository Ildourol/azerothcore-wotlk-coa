CLI_DESCRIPTION = """Check the CoA talent budget table and the spellbook-derived talent state without a server.

Compiles the module's talent catalog loader and talent state code against the client DBC set the server loads
(--dbc-dir), then checks the essence budgets, rank derivation, point accounting, the known-entries wire form and
the specialization a known-entries upload selects.
No database, server build or game client is needed.
"""

import argparse
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
from coa_talent_catalog import STUBS  # noqa: E402
from client_data import dbc_dir  # noqa: E402

MAIN = r"""
#include "AscensionCoATalentData.h"
#include "AscensionCoATalentState.h"
#include "DBCStores.h"
#include <algorithm>
#include <cstdio>
#include <set>

using namespace AscensionCompatData;
using namespace AscensionCoATalentState;

namespace
{
int failures = 0;
void Check(bool value, char const* name)
{
    failures += !value;
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
}

CoATalentEntry const* Find(std::uint32_t entryId)
{
    for (CoATalentEntry const& entry : CoATalentEntries)
        if (entry.EntryId == entryId)
            return &entry;
    return nullptr;
}

// The sweep the realm runs on every build it accepts (`ValidateTalentState`): a row whose own
// investment gates the build no longer satisfies is unlearned, to a fixpoint. Dropping a row only
// takes investment away from the rows that count it, so the sweep settles instead of oscillating.
std::vector<KnownEntry> Sweep(std::vector<KnownEntry> build)
{
    for (bool changed = true; changed;)
    {
        std::vector<std::uint32_t> const failing = UnsatisfiedInvestmentGates(build);
        changed = !failing.empty();
        for (std::uint32_t const entryId : failing)
            std::erase_if(build, [entryId](KnownEntry const& item) { return item.EntryId == entryId; });
    }
    return build;
}

// The first paid entry of a class on a tree, with at least `ranks` ranks.
CoATalentEntry const* FirstPaid(std::uint8_t classId, bool classTree, std::uint32_t ranks = 1)
{
    for (CoATalentEntry const& entry : CoATalentEntries)
        if (entry.ClassId == classId && (entry.AECost || entry.TECost) && (entry.SpecId == 0) == classTree &&
            entry.SpellCount >= ranks)
            return &entry;
    return nullptr;
}
}

int main(int, char** argv)
{
    DbcDirectory = std::string(argv[1]) + "/";
    Check(LoadCoATalentData(), "catalog loads with the essence table");

    // Budgets: one class point and one specialization point from level 10 on, alternating upwards.
    // Talents unlock at 10 with a point to spend in each tree; the baseline itself is free.
    std::uint32_t ae = 0, te = 0;
    bool everyClass = true;
    for (std::uint8_t classId = 12; classId <= 32; ++classId)
    {
        everyClass = everyClass && GetCoATalentBudget(classId, 10, ae, te) && ae == 1 && te == 1;
        everyClass = everyClass && GetCoATalentBudget(classId, 11, ae, te) && ae == 1 && te == 1;
        everyClass = everyClass && GetCoATalentBudget(classId, 60, ae, te) && ae == 26 && te == 25;
        everyClass = everyClass && GetCoATalentBudget(classId, 80, ae, te) && ae == 36 && te == 35;
    }
    Check(everyClass, "every custom class has the 1/1, 1/1, 26/25 and 36/35 budgets at levels 10, 11, 60, 80");
    Check(GetCoATalentBudget(30, 9, ae, te) && ae == 0 && te == 0, "level 9 holds no points");
    Check(GetCoATalentBudget(30, 255, ae, te) && ae == 36 && te == 35, "a level past the table keeps the last row");
    Check(!GetCoATalentBudget(1, 60, ae, te), "a native class has no budget row");

    // Ranks and points from a spellbook.
    CoATalentEntry const* three = nullptr;
    for (CoATalentEntry const& entry : CoATalentEntries)
        if ((entry.AECost || entry.TECost) && entry.SpellCount == 3 && entry.SpecId == 0)
        {
            three = &entry;
            break;
        }
    Check(three != nullptr, "a paid three-rank class talent exists");
    CoATalentEntry const* spec = three ? FirstPaid(three->ClassId, false) : nullptr;
    Check(spec != nullptr, "the same class has a paid specialization talent");
    if (three && spec)
    {
        std::set<std::uint32_t> spellbook = { three->SpellIds[1], spec->SpellIds[0] };
        HasSpell hasSpell = [&spellbook](std::uint32_t id) { return spellbook.count(id) != 0; };
        Check(KnownRank(*three, hasSpell) == 2, "rank is the highest owned rank spell");
        Check(KnownRank(*spec, hasSpell) == 1, "a single owned rank spell is rank 1");
        std::vector<KnownEntry> known = KnownEntries(three->ClassId, hasSpell);
        Check(known.size() == 2, "known entries list exactly the owned entries");
        SpentPoints spent = Spent(known);
        Check(spent.AE == 2 * three->AECost && spent.TE == spec->TECost,
              "spent points charge every rank on the tree it belongs to");
        Check(Spent({}).AE == 0 && Spent({}).TE == 0, "nothing owned spends nothing");
        Check(KnownEntries(three->ClassId, [](std::uint32_t) { return false; }).empty(),
              "an empty spellbook knows no entry");

        // Shared rank spell: charged once, to the lower entry id (7131 and 12264 share 503748 in the catalog).
        CoATalentEntry const* first = Find(7131);
        CoATalentEntry const* second = Find(12264);
        if (first && second && first->SpellIds[0] == second->SpellIds[0])
        {
            std::vector<KnownEntry> shared = { { 7131, 1 }, { 12264, 1 } };
            SpentPoints once = Spent(shared);
            Check(once.AE + once.TE == 1, "a rank spell two entries share is charged once");
            Check(once.TE == 1 && once.AE == 0, "the shared spell is charged to the lower entry id's tree");
        }
        else
            Check(false, "catalog still shares spell 503748 between entries 7131 and 12264");

        // Wire form.
        std::vector<std::uint8_t> body = KnownEntriesPayload(known);
        Check(body.size() == 4 + 21 * known.size(), "known-entries body is u32 count plus 21 bytes per record");
        Check(body[0] == 2 && body[1] == 0 && body[2] == 0 && body[3] == 0, "count is little-endian");
        std::vector<KnownEntry> parsed;
        Check(ParseKnownEntriesUpload(body.data(), body.size(), parsed) && parsed.size() == 2 &&
                  parsed[0].EntryId == known[0].EntryId && parsed[0].Rank == known[0].Rank &&
                  parsed[1].EntryId == known[1].EntryId && parsed[1].Rank == known[1].Rank,
              "the upload parser reads back what the payload wrote");
        Check(!ParseKnownEntriesUpload(body.data(), body.size() - 1, parsed), "a short body is refused");
        body.push_back(0);
        Check(!ParseKnownEntriesUpload(body.data(), body.size(), parsed), "a long body is refused");
        std::uint8_t empty[4] = { 0, 0, 0, 0 };
        Check(ParseKnownEntriesUpload(empty, 4, parsed) && parsed.empty(), "count zero is an empty set");
        Check(!ParseKnownEntriesUpload(empty, 3, parsed), "less than a count is refused");
        Check(KnownEntriesPayload({}).size() == 4, "an empty set is a bare zero count");
    }

    std::size_t abilities = 0, talents = 0, otherTypes = 0;
    for (CoATalentEntry const& entry : CoATalentEntries)
    {
        if (IsAbilityLike(entry)) ++abilities;
        else if (IsTalent(entry)) ++talents;
        else ++otherTypes;
    }
    std::printf("catalog row types: %zu ability-like, %zu talent, %zu other\n", abilities, talents, otherTypes);
    Check(abilities > 0, "the catalog carries rows the client types as abilities");

    std::size_t baselineRows = 0, baselineAbilities = 0;
    for (CoASpecialization const& specialization : CoASpecializations)
        for (KnownEntry const& item : DefaultEntries(specialization.ClassId, specialization.SpecId))
        {
            ++baselineRows;
            CoATalentEntry const* entry = Find(item.EntryId);
            if (entry && IsAbilityLike(*entry))
                ++baselineAbilities;
        }
    std::printf("baseline rows: %zu, of them ability-like: %zu\n", baselineRows, baselineAbilities);
    Check(baselineAbilities > 0,
          "archetype baseline rows are ability-typed too, so an abilities purge that ignored "
          "defaults would ask the realm to drop a build it must keep");

    // Unlearn pricing reads the row's own free-to-unlearn bit, the bit the client masks
    // (`RowHasFlag(r, 0x2000)`, AscCARules.cpp `UnlearnCost`); being in a default set is not the
    // test, because spec 5's own default 31154 Shadow Puppets carries no bit and the client bills
    // it. A misread offset is the failure this guards: it would make the bit constant, or make it
    // disagree with the rows the archetypes grant.
    std::size_t freeRows = 0;
    for (CoATalentEntry const& entry : CoATalentEntries)
        freeRows += IsCoAFreeToUnlearnRow(entry.EntryId) ? 1 : 0;
    std::printf("catalog rows free to unlearn: %zu of %zu\n", freeRows, CoATalentEntries.size());
    Check(freeRows > 0 && freeRows < CoATalentEntries.size(),
          "the free-to-unlearn bit is a minority of the catalog, not a constant");
    bool identitiesFree = true;
    for (CoASpecialization const& specialization : CoASpecializations)
        identitiesFree = identitiesFree && IsCoAFreeToUnlearnRow(specialization.IdentityEntryId);
    Check(identitiesFree, "every archetype's identity row is free to unlearn");
    Check(IsCoAFreeToUnlearnRow(29744) && IsCoAFreeToUnlearnRow(4005) && !IsCoAFreeToUnlearnRow(31154),
          "granted rows carry the bit while a billed default row does not");

    // A baseline is the signature, the identity, the archetype's own opening ranks and the granted
    // free rows they unlock - nothing else. The opening ranks are the archetype's even where the
    // data does not mark them free to unlearn (spec 5's 31154 is a spend circle costing one TE):
    // entering grants them, and leaving prices them, which is what an archetype switch costs with
    // nothing spent. Every other baseline row must be one the data grants, or the realm would hand
    // over a row the window neither holds nor prices.
    bool baselineHoldsOnlyDefaults = true;
    std::size_t openingRows = 0, baselineRowsChecked = 0, billedOpeners = 0;
    for (CoASpecialization const& specialization : CoASpecializations)
    {
        openingRows += specialization.OpeningRowEntryIds.size();
        for (KnownEntry const& item : DefaultEntries(specialization.ClassId, specialization.SpecId))
        {
            ++baselineRowsChecked;
            bool const granted = IsCoAGrantedRow(item.EntryId);
            bool const opener = std::count(specialization.OpeningRowEntryIds.begin(),
                                           specialization.OpeningRowEntryIds.end(), item.EntryId) != 0;
            if (!granted)
                ++billedOpeners;
            baselineHoldsOnlyDefaults = baselineHoldsOnlyDefaults &&
                (granted || opener || item.EntryId == specialization.SignatureEntryId ||
                 item.EntryId == specialization.IdentityEntryId);
        }
    }
    std::printf("opening rows: %zu, baseline rows: %zu, of them billed on departure: %zu\n", openingRows,
                baselineRowsChecked, billedOpeners);
    Check(baselineHoldsOnlyDefaults,
          "a baseline holds the signature, the identity, the archetype's own opening ranks and "
          "granted free rows only");
    Check(billedOpeners > 0,
          "archetypes do open on ranks the data does not mark free - what a departure costs with "
          "no points spent");
    Check(IsCoAGrantedRow(4533) && IsCoAGrantedRow(4532) && IsCoAGrantedRow(9311) && IsCoAGrantedRow(4005) &&
              IsCoAGrantedRow(29744) && !IsCoAGrantedRow(31154) && !IsCoAGrantedRow(6047) &&
              !IsCoAGrantedRow(6054) && !IsCoAGrantedRow(29309),
          "the auto-learn bit marks the passives the archetypes hand out, and the spend circles stay "
          "purchases");
    {
        CoASpecialization const* voodoo = nullptr;
        for (CoASpecialization const& specialization : CoASpecializations)
            if (specialization.SignatureEntryId == 29301)
                voodoo = &specialization;
        std::size_t rows = 0;
        bool pairAndOpener = voodoo != nullptr, openerBilled = false;
        if (voodoo)
            for (KnownEntry const& item : DefaultEntries(voodoo->ClassId, voodoo->SpecId))
            {
                ++rows;
                pairAndOpener = pairAndOpener && (item.EntryId == voodoo->SignatureEntryId ||
                                                  item.EntryId == voodoo->IdentityEntryId ||
                                                  item.EntryId == 31154);
                if (item.EntryId == 31154)
                    openerBilled = !IsCoAFreeToUnlearnRow(31154);
            }
        Check(rows == 3 && pairAndOpener && openerBilled,
              "Voodoo's baseline is its signature, its identity and 31154 Voodoo Witch Doctor, and "
              "that rank is exactly what leaving Voodoo costs with no points spent");
    }

    std::size_t purged = 0;
    bool purgeKeepsBaseline = true;
    for (CoASpecialization const& specialization : CoASpecializations)
    {
        CoATalentEntry const* ability = nullptr;
        for (CoATalentEntry const& entry : CoATalentEntries)
            if (entry.ClassId == specialization.ClassId &&
                (entry.SpecId == specialization.SpecId || entry.SpecId == 0) && IsAbilityLike(entry) &&
                !IsDefaultEntry(specialization.ClassId, specialization.SpecId, entry.EntryId))
            {
                ability = &entry;
                break;
            }
        if (!ability)
            continue;
        std::vector<KnownEntry> build = DefaultEntries(specialization.ClassId, specialization.SpecId);
        build.push_back({ ability->EntryId, 1 });
        std::size_t const beforePurge = build.size();
        std::erase_if(build, [&specialization](KnownEntry const& item)
        {
            CoATalentEntry const* entry = Find(item.EntryId);
            return entry && IsAbilityLike(*entry) &&
                !IsDefaultEntry(specialization.ClassId, specialization.SpecId, item.EntryId);
        });
        ++purged;
        purgeKeepsBaseline = purgeKeepsBaseline && build.size() == beforePurge - 1 &&
            std::none_of(build.begin(), build.end(),
                [ability](KnownEntry const& item) { return item.EntryId == ability->EntryId; });
    }
    std::printf("archetypes with a paid ability row to purge: %zu\n", purged);
    Check(purged > 0 && purgeKeepsBaseline,
          "the abilities purge drops a paid ability row and leaves the archetype's baseline");

    std::set<std::uint8_t> specializedClasses;
    for (CoATalentEntry const& entry : CoATalentEntries)
        if (entry.SpecId)
            specializedClasses.insert(entry.ClassId);
    std::set<std::uint8_t> identifiedClasses;
    bool identitiesBelong = !CoASpecializations.empty();
    bool switchesDetected = true;
    for (CoASpecialization const& specialization : CoASpecializations)
    {
        identifiedClasses.insert(specialization.ClassId);
        CoATalentEntry const* identity = Find(specialization.IdentityEntryId);
        identitiesBelong = identitiesBelong && identity && identity->ClassId == specialization.ClassId &&
            identity->SpecId == specialization.SpecId;
        UploadedSpecialization const uploaded = SpecializationOf(
            SpecializationSwitch(specialization.ClassId, [](std::uint32_t) { return false; }, specialization.SpecId));
        switchesDetected = switchesDetected && uploaded.SpecId == specialization.SpecId && !uploaded.Mixed &&
            !uploaded.ChoosesTalents;
    }
    Check(identitiesBelong,
          "every specialization identity entry is a catalog entry of its own class and specialization");
    Check(identifiedClasses == specializedClasses, "every class with specialization talents has identity entries");
    Check(switchesDetected,
          "a switch upload names exactly the specialization it enters and chooses none of its talents");

    CoATalentEntry const* classTalent = spec ? FirstPaid(spec->ClassId, true) : nullptr;
    std::vector<CoASpecialization> specializations;
    for (CoASpecialization const& specialization : CoASpecializations)
        if (classTalent && specialization.ClassId == classTalent->ClassId)
            specializations.push_back(specialization);
    Check(classTalent && specializations.size() >= 2, "a class has a paid class talent and two specializations");
    if (classTalent && specializations.size() >= 2)
    {
        std::set<std::uint32_t> spellbook = { classTalent->SpellIds[0] };
        HasSpell hasSpell = [&spellbook](std::uint32_t id) { return spellbook.count(id) != 0; };
        std::vector<KnownEntry> const entering =
            SpecializationSwitch(classTalent->ClassId, hasSpell, specializations[0].SpecId);
        Check(entering.size() == 2 + specializations[0].OpeningRowEntryIds.size() &&
                  std::all_of(entering.begin(), entering.end(),
                  [&specializations, classTalent](KnownEntry const& item)
                  { return IsDefaultEntry(classTalent->ClassId, specializations[0].SpecId, item.EntryId); }),
              "a switch upload discards the old class tree and contains only the new baseline");
        Check(SpecializationOf({ { classTalent->EntryId, 1 } }).SpecId == 0,
              "a class-tree upload names no specialization");
        Check(SpecializationOf({ { specializations[0].IdentityEntryId, 0 } }).SpecId == 0,
              "an entry at rank 0 names no specialization");
        Check(SpecializationOf({ { specializations[0].IdentityEntryId, 1 },
                  { specializations[1].IdentityEntryId, 1 } }).Mixed,
              "entries of two specializations are a mixed upload");

        CoATalentEntry const* chosen = nullptr;
        CoATalentEntry const* automatic = nullptr;
        for (CoATalentEntry const& entry : CoATalentEntries)
        {
            if (entry.SpecId != specializations[0].SpecId)
                continue;
            if (!chosen && (entry.AECost || entry.TECost) && entry.EntryId != specializations[0].SignatureEntryId)
                chosen = &entry;
            if (!automatic && !entry.AECost && !entry.TECost && entry.EntryId != specializations[0].IdentityEntryId)
                automatic = &entry;
        }
        Check(chosen && automatic, "the specialization has a paid talent and an automatic entry");
        if (chosen && automatic)
        {
            UploadedSpecialization const picked = SpecializationOf({ { chosen->EntryId, 1 } });
            Check(picked.SpecId == specializations[0].SpecId && picked.ChoosesTalents,
                  "a paid specialization talent names its specialization and chooses a talent");
            Check(SpecializationOf({ { automatic->EntryId, 1 } }).SpecId == specializations[0].SpecId,
                  "a selected zero-cost row names its own specialization");
            Check(SpecializationOf({ { automatic->EntryId, 1 }, { specializations[1].IdentityEntryId, 1 } }).Mixed,
                  "a selected zero-cost row of another specialization mixes the upload");
        }
    }

    // Investment gates: `MeetsInvestmentForAddByEntryID`'s slots 38/39/40 - the rule that
    // paints a held row red and refuses its right-click refund. A row never counts towards
    // its own tier, so a gated row alone can never satisfy itself, and a set the gates do
    // not support has to be pruned before it is pushed.
    {
        std::size_t gatedRows = 0;
        CoATalentEntry const* gated = nullptr;
        for (CoATalentEntry const& entry : CoATalentEntries)
        {
            bool const gatedRow = entry.GateAE[0] || entry.GateAE[1] || entry.GateAE[2] ||
                entry.GateTE[0] || entry.GateTE[1] || entry.GateTE[2] || entry.PointsGate;
            gatedRows += gatedRow;
            if (!gated && gatedRow && (entry.AECost || entry.TECost))
                gated = &entry;
        }
        Check(gatedRows > 0, "the catalog carries investment gates");
        Check(gated != nullptr, "a paid talent is behind an investment gate");
        if (gated)
        {
            std::vector<KnownEntry> const alone = { { gated->EntryId, 1 } };
            std::vector<std::uint32_t> const flagged = UnsatisfiedInvestmentGates(alone);
            Check(flagged.size() == 1 && flagged[0] == gated->EntryId,
                  "a gated row on its own is flagged: a row never counts towards its own tier");
            Check(!InvestmentGateShortfall(gated->EntryId, alone).empty(),
                  "the shortfall is named for the log");

            Check(Sweep(alone).empty(), "sweeping the gates to a fixpoint removes the unsupported row");
        }

        // The state every archetype opens on is what the realm grants - the signature, the
        // identity and the opening ranks - and it has to satisfy its own gates, or the tree
        // is drawn with a red node the player cannot clear.
        bool defaultsSatisfied = !CoASpecializations.empty();
        for (CoASpecialization const& specialization : CoASpecializations)
        {
            std::vector<KnownEntry> defaults =
                DefaultEntries(specialization.ClassId, specialization.SpecId);
            defaultsSatisfied = defaultsSatisfied && !defaults.empty() &&
                UnsatisfiedInvestmentGates(defaults).empty();
        }
        Check(defaultsSatisfied, "every archetype's default state satisfies its own investment gates");
    }

    // The reported shape: refunding a row that a HELD row is gated behind. Mojo Beam (30823) is
    // Brewing's, costs 1 TE and needs 8 TE already invested in its own tree; Potent Mixes (7131)
    // is a 1-TE row of that same tree. The window's own replay would refuse a build that holds
    // the first without the second's tree investment - but `ValidateApply` skips that replay for
    // a diff that only removes rows, and a refund is exactly that, so the build went through and
    // the realm went on granting the spell. The realm is the authority for what it holds, so this
    // is the state the server-side check has to refuse.
    {
        CoATalentEntry const* mojo = Find(30823);
        CoATalentEntry const* mixes = Find(7131);
        std::uint32_t brewingClass = 0, brewingSpec = 0;
        for (CoASpecialization const& specialization : CoASpecializations)
            if (specialization.IdentityEntryId == 4005)   // Cauldron Brewer, the Brewing identity
            {
                brewingClass = specialization.ClassId;
                brewingSpec = specialization.SpecId;
            }
        std::uint16_t const need = mojo ? mojo->GateTE[2] : 0;
        Check(mojo && mixes && need && brewingSpec && mojo->TabId == mixes->TabId,
              "the reported pair is in the catalog: Mojo Beam gated on its own tree, Potent Mixes "
              "a 1-item row of the same tree");
        std::uint32_t const mixesRank = 2;   // the rank the report refunded
        if (mojo && mixes && need > mixesRank && brewingSpec)
        {
            // The tree's own investment, one Potent Mixes short of the gate: built from the rows
            // that count towards it (a row whose own gate is at or above the threshold is skipped,
            // so only the tree's ungated rows count), each of them one item, so the arithmetic is
            // the player's and not the harness's.
            std::vector<KnownEntry> build = DefaultEntries(brewingClass, brewingSpec);
            std::size_t const baseline = build.size();
            for (CoATalentEntry const& entry : CoATalentEntries)
                if (entry.ClassId == mojo->ClassId && entry.TabId == mojo->TabId && entry.TECost == 1 &&
                    !entry.GateTE[2] && entry.EntryId != mojo->EntryId && entry.EntryId != mixes->EntryId &&
                    build.size() - baseline < need - mixesRank)
                    build.push_back({ entry.EntryId, 1 });
            Check(build.size() - baseline == need - mixesRank,
                  "the tree holds enough ungated rows to build the reported refund exactly");
            Check(!InvestmentGateShortfall(mojo->EntryId, build).empty(),
                  "one Potent Mixes short of the gate the build is short: the gate is measured, not guessed");
            std::vector<KnownEntry> held = build;
            held.push_back({ mixes->EntryId, mixesRank });
            held.push_back({ mojo->EntryId, 1 });
            Check(InvestmentGateShortfall(mojo->EntryId, held).empty(),
                  "Mojo Beam's own-tree gate is met while Potent Mixes is held");
            std::vector<KnownEntry> refunded = build;
            refunded.push_back({ mojo->EntryId, 1 });
            std::vector<std::uint32_t> const unsatisfied = UnsatisfiedInvestmentGates(refunded);
            Check(!InvestmentGateShortfall(mojo->EntryId, refunded).empty() &&
                  std::find(unsatisfied.begin(), unsatisfied.end(), mojo->EntryId) != unsatisfied.end(),
                  "refunding Potent Mixes leaves Mojo Beam behind an unmet gate: the build the "
                  "realm used to accept, and the spell it kept granting");
            // What the realm has to do with that build: unlearn the row the refund left unsupported,
            // so its spell goes with it, and take nothing else off the tree on the way.
            std::vector<KnownEntry> const healed = Sweep(refunded);
            Check(std::none_of(healed.begin(), healed.end(), [mojo](KnownEntry const& item)
                      { return item.EntryId == mojo->EntryId; }),
                  "the sweep unlearns Mojo Beam: the realm stops holding the rank, and its spell");
            Check(UnsatisfiedInvestmentGates(healed).empty(),
                  "the build the sweep leaves satisfies every gate it kept");
            std::vector<KnownEntry> const bare = Sweep(build);
            bool onlyMojo = healed.size() == bare.size();
            for (KnownEntry const& item : healed)
                onlyMojo = onlyMojo && std::any_of(bare.begin(), bare.end(),
                    [&item](KnownEntry const& other) { return other.EntryId == item.EntryId; });
            Check(onlyMojo, "dropping Mojo Beam condemns nothing else: the sweep is one row, not a cascade");
        }
    }

    bool baselineShape = !CoASpecializations.empty();
    bool freeDefaults = baselineShape;
    bool uniqueSpells = baselineShape;
    bool openersDefault = baselineShape;
    for (auto const& specialization : CoASpecializations)
    {
        auto const defaults = DefaultEntries(specialization.ClassId, specialization.SpecId);
        bool distinct = defaults.size() == 2 + specialization.OpeningRowEntryIds.size();
        for (auto const& item : defaults)
            distinct = distinct && item.Rank == 1 &&
                std::count_if(defaults.begin(), defaults.end(), [&item](auto const& other)
                    { return other.EntryId == item.EntryId; }) == 1;
        baselineShape = baselineShape && distinct;
        auto const spent = Spent(defaults, specialization.ClassId, specialization.SpecId);
        freeDefaults = freeDefaults && spent.AE == 0 && spent.TE == 0;
        auto repeated = defaults;
        repeated.insert(repeated.end(), defaults.begin(), defaults.end());
        uniqueSpells = uniqueSpells && SelectedSpells(repeated) == SelectedSpells(defaults);
        for (auto const entryId : specialization.OpeningRowEntryIds)
            openersDefault = openersDefault &&
                IsDefaultEntry(specialization.ClassId, specialization.SpecId, entryId);
    }
    Check(baselineShape, "every archetype's baseline is the signature, the identity and its opening "
          "ranks, each once at rank 1");
    Check(freeDefaults, "the whole baseline costs zero for every archetype");
    Check(uniqueSpells, "repeated entry states never produce duplicate spell grants");
    Check(openersDefault, "every opening rank is part of the archetype's default state");

    // The split that decides how a build may be treated: a signature lives on the shared class
    // tree (no archetype owns it) while an identity and the opening ranks live on the archetype's
    // own tree. Rows of an archetype's own tree are the ones an upload is refused for holding on
    // the wrong archetype; the shared signature row is a class talent the window sells and the
    // character keeps, whichever archetype it belongs to.
    bool signaturesUnowned = !CoASpecializations.empty();
    bool openersOwned = signaturesUnowned;
    for (CoASpecialization const& specialization : CoASpecializations)
    {
        CoATalentEntry const* signature = Find(specialization.SignatureEntryId);
        signaturesUnowned = signaturesUnowned && signature && signature->SpecId == 0;
        for (std::uint32_t opener : specialization.OpeningRowEntryIds)
        {
            CoATalentEntry const* row = Find(opener);
            openersOwned = openersOwned && row && row->SpecId == specialization.SpecId && row->SpecId != 0;
        }
    }
    Check(signaturesUnowned,
          "an archetype's signature is a shared class row no archetype owns - the window sells it, so the "
          "character keeps it across an upload");
    Check(openersOwned,
          "every opening rank sits on its own archetype's tree - what makes a rank of another archetype's "
          "tree a refusal rather than a silent drop");

    Check(DefaultEntries(1, 0).empty(), "an unknown archetype has no inferred defaults");

    {
        Check(TalentStateRevision == 4 && TalentStateStride(1) == 2 && TalentStateStride(2) == 3 &&
                  TalentStateStride(3) == 3 && TalentStateStride(4) == 3 && TalentStateStride(5) == 0,
              "a revision-1 to revision-3 talent state still reads at its own stride, and only 1/2/3/4 read");
        Check(LoadoutRecordSlots == 30 && LoadoutRecordEntriesSlots == 1 + LoadoutMaxEntries * 3 &&
                  LoadoutRecordSlotsV4 == 30 + LoadoutRecordEntriesSlots &&
                  LoadoutRecordStride(3) == 30 && LoadoutRecordStride(4) == LoadoutRecordSlotsV4 &&
                  LoadoutRecordStride(2) == 0 &&
                  LoadoutBlockSlots(0) == 12 && LoadoutBlockSlots(2) == 2 * LoadoutRecordSlotsV4 + 12 &&
                  LoadoutBlockSlots(2, LoadoutRecordSlots) == 2 * 30 + 12,
              "the loadout record keeps the 30-slot prefix, a counted entries block per loadout "
              "and the 11-slot active uuid; the record stride follows the revision");

        std::vector<Loadout> loadouts = {
            { "2f4a1c9e-0dc5-4c67-9d0e-8a5c4f2a1b30", "PvP \"main\"", 3, 41,
                { { 7131, 2, 0, false, 0 }, { 12264, 1, 0, true, 0 } } },
            { "0b1d2c3e-4f50-4a6b-8c7d-9e0f1a2b3c4d", "Leveling", 0, 96, {} },
        };
        std::string const active = loadouts[0].Uuid;
        std::vector<std::uint32_t> const block = BuildLoadoutBlock(loadouts, active);
        Check(block.size() == LoadoutBlockSlots(loadouts.size()) && block[0] == loadouts.size(),
              "the loadout block is a count, one fixed record per loadout and the active uuid");
        std::vector<Loadout> readBack;
        std::string readActive;
        std::size_t const v4 = LoadoutRecordStride(TalentStateRevision);
        Check(ParseLoadoutBlock(block.data(), block.size(), v4, readBack, readActive) &&
                  readBack.size() == loadouts.size() && readActive == active &&
                  readBack[0].Uuid == loadouts[0].Uuid && readBack[0].Name == loadouts[0].Name &&
                  readBack[0].SortOrder == 3 && readBack[0].SpecId == 41 &&
                  readBack[0].Entries.size() == 2 && readBack[0].Entries[0].EntryId == 7131 &&
                  readBack[0].Entries[0].Rank == 2 && !readBack[0].Entries[0].Locked &&
                  readBack[0].Entries[1].EntryId == 12264 && readBack[0].Entries[1].Rank == 1 &&
                  readBack[0].Entries[1].Locked &&
                  readBack[1].Name == loadouts[1].Name && readBack[1].SortOrder == 0 &&
                  readBack[1].SpecId == 96 && readBack[1].Entries.empty(),
              "the loadout block round-trips uuid, name, sort order, archetype, stored entries "
              "and the active uuid");
        Check(ParseLoadoutBlock(block.data(), block.size() - 1, v4, readBack, readActive) == false &&
                  readBack.empty() && readActive.empty(),
              "a truncated loadout block loads nothing rather than half a list");
        Check(ParseLoadoutBlock(block.data(), block.size() - LoadoutActiveSlots, v4,
                  readBack, readActive) == false,
              "a loadout block that stops before the active uuid is refused");
        Check(!ParseLoadoutBlock(nullptr, 0, v4, readBack, readActive) &&
                  !ParseLoadoutBlock(block.data(), 0, v4, readBack, readActive),
              "a state written before loadouts existed carries no loadouts and still reads its build");

        /* A revision-3 writer laid its records out at 30 slots with no entries block, so
         * its whole block is 97 slots per record smaller; replaying that exact layout
         * and reading it at the revision-3 stride is the migration the realm performs. */
        std::size_t const legacyCount = 2;
        std::vector<std::uint32_t> legacy(LoadoutBlockSlots(legacyCount, LoadoutRecordSlots), 0);
        legacy[0] = std::uint32_t(legacyCount);
        std::vector<std::uint32_t> firstHeader = BuildLoadoutBlock(
            { { loadouts[0].Uuid, loadouts[0].Name, loadouts[0].SortOrder, loadouts[0].SpecId, {} } },
            loadouts[0].Uuid);
        std::vector<std::uint32_t> secondHeader = BuildLoadoutBlock(
            { { loadouts[1].Uuid, loadouts[1].Name, loadouts[1].SortOrder, loadouts[1].SpecId, {} } },
            loadouts[1].Uuid);
        for (std::size_t index = 0; index < LoadoutRecordSlots; ++index)
            legacy[1 + index] = firstHeader[1 + index];
        for (std::size_t index = 0; index < LoadoutRecordSlots; ++index)
            legacy[1 + LoadoutRecordSlots + index] = secondHeader[1 + index];
        for (std::size_t index = 0; index < LoadoutActiveSlots; ++index)
            legacy[1 + legacyCount * LoadoutRecordSlots + index] =
                firstHeader[1 + LoadoutRecordSlotsV4 + index];
        std::vector<Loadout> legacyBack;
        std::string legacyActive;
        Check(ParseLoadoutBlock(legacy.data(), legacy.size(), LoadoutRecordStride(3),
                  legacyBack, legacyActive) &&
                  legacyBack.size() == legacyCount && legacyActive == active &&
                  legacyBack[0].Uuid == loadouts[0].Uuid && legacyBack[0].SpecId == 41 &&
                  legacyBack[0].Entries.empty() && legacyBack[1].Uuid == loadouts[1].Uuid &&
                  legacyBack[1].Entries.empty(),
              "a revision-3 loadout block (no per-record entries) still reads with empty lists");
        Check(ParseLoadoutBlock(legacy.data(), legacy.size(), v4, legacyBack, legacyActive) == false,
              "a revision-3 block is refused at the revision-4 stride rather than read past its end");

        std::vector<KnownEntry> manyEntries;
        for (std::size_t index = 0; index < LoadoutMaxEntries + 5; ++index)
            manyEntries.push_back({ std::uint32_t(7000 + index), std::uint32_t(index % 3 + 1), 0, false, 0 });
        std::vector<Loadout> packed = { { loadouts[0].Uuid, "packed", 0, 41, manyEntries } };
        std::vector<std::uint32_t> const packedBlock = BuildLoadoutBlock(packed, packed[0].Uuid);
        std::vector<Loadout> packedBack;
        std::string packedActive;
        Check(packedBlock.size() == LoadoutBlockSlots(1) &&
                  ParseLoadoutBlock(packedBlock.data(), packedBlock.size(), v4,
                      packedBack, packedActive) &&
                  packedBack[0].Entries.size() == LoadoutMaxEntries &&
                  packedBack[0].Entries[LoadoutMaxEntries - 1].EntryId ==
                      std::uint32_t(7000 + LoadoutMaxEntries - 1),
              "a stored entry list is kept at the documented per-loadout maximum");

        std::vector<Loadout> longStrings = { { std::string(120, 'u'), std::string(300, 'n'), 1, 40, {} } };
        std::vector<Loadout> longBack;
        std::string longActive;
        std::vector<std::uint32_t> const longBlock = BuildLoadoutBlock(longStrings, longStrings[0].Uuid);
        Check(ParseLoadoutBlock(longBlock.data(), longBlock.size(), v4, longBack, longActive) &&
                  longBack.size() == 1 && longBack[0].Uuid.size() == LoadoutUuidBytes &&
                  longBack[0].Name.size() == LoadoutNameBytes && longActive.size() == LoadoutUuidBytes,
              "an over-long uuid or name is stored at the documented byte bound, never past its record");

        std::vector<Loadout> many;
        for (std::size_t index = 0; index < MaxLoadouts + 4; ++index)
            many.push_back({ "u" + std::to_string(index), "n" + std::to_string(index),
                std::uint32_t(index), 40, {} });
        std::vector<std::uint32_t> const full = BuildLoadoutBlock(many, many[0].Uuid);
        std::vector<Loadout> fullBack;
        std::string fullActive;
        Check(full.size() == LoadoutBlockSlots(MaxLoadouts) &&
                  ParseLoadoutBlock(full.data(), full.size(), v4, fullBack, fullActive) &&
                  fullBack.size() == MaxLoadouts && fullActive == many[0].Uuid,
              "the loadout list is stored and read back at the documented maximum");
    }
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=CLI_DESCRIPTION)
    parser.add_argument("--dbc-dir", type=Path)
    args = parser.parse_args()
    args.dbc_dir = args.dbc_dir or dbc_dir()

    compiler = shutil.which(os.environ.get("CXX", "cl.exe" if os.name == "nt" else "c++"))
    assert compiler, "Enable a C++20 compiler (VS Developer PowerShell on Windows)."

    method = runpy.run_path(str(HERE.parent / "client_compat/run.py"))["method"]
    validate = method((ROOT / "src/server/coa/AscensionCompat.cpp").read_text(encoding="utf-8"),
                      "bool ValidateTalentState(")
    assert "InvestmentGateShortfall" in validate, (
        "the realm's validation no longer consults the investment gates: an upload that keeps a "
        "gated row without the investment it needs is accepted again")
    assert validate.index("InvestmentGateShortfall(item.EntryId, desired)") < validate.index("if (!checkBudget)"), (
        "the gate sweep is not reached before the budget gate's early return, so the two paths that "
        "ask for a build without a budget check - the restore that rebuilds one from the spell book "
        "on login and the reset that forgets one - go on storing a build that does not satisfy its "
        "own gates until the character's next save")
    assert "std::erase_if(desired" in validate, (
        "the realm's validation stopped settling the build: a row the request no longer supports has "
        "to be unlearned, and refusing it instead leaves the window holding a build the realm will "
        "not take (and cannot refund its way out of)")
    source = (ROOT / "src/server/coa/AscensionCompat.cpp").read_text(encoding="utf-8")
    apply = method(source, "bool ApplyTalentState(")
    assert "ValidateTalentState(player, specializationId, desired, refusal, checkBudget, &pruned)" in apply, (
        "the realm's apply no longer collects the rows validation had to unlearn, so the player is "
        "never told which rank left the build")
    assert "PayUnlearnCosts(player, previousSpec, specializationId, previousState, requested, refusal)" in apply, (
        "the realm prices the settled build instead of the requested one: the charge stops matching "
        "the confirmation the window drew")

    assert "SpecializationSwitchRefusal(player, previousSpec, specializationId)" in apply, (
        "the realm stopped consulting the archetype switch guards where it changes archetype: a "
        "prestige-locked character could switch again")
    assert apply.index("SpecializationSwitchRefusal(") < apply.index("PayUnlearnCosts("), (
        "the realm charges for a switch before it asks whether the switch is allowed")
    assert apply.index("SpecializationSwitchRefusal(") < apply.index("StoreTalentState("), (
        "the realm stores the new archetype before it asks whether the switch is allowed")
    cleared = method(source, "void ClearTalentState(Player* player)")
    writes = source.count("UpdatePlayerSetting(ASCENSION_ACTIVE_SPEC_SETTING")
    assert writes == 1 + cleared.count("UpdatePlayerSetting(ASCENSION_ACTIVE_SPEC_SETTING"), (
        "the archetype a character is on is written somewhere other than the apply path and the "
        "whole-state clear: a writer the switch guards are not on is a way past them")

    prestige = (ROOT / "modules/mod-coa-prestige/src/CoAPrestige.cpp").read_text(encoding="utf-8")
    assert "AddAscensionSpecializationSwitchGuard(SpecializationSwitchRefusal)" in prestige, (
        "Prestige Mode no longer registers its specialization lock, so the guard the realm consults "
        "has no holder and a character in Prestige Mode can switch archetypes again")
    change = (ROOT / "modules/mod-coa-change-potions/src/change_potions.cpp").read_text(encoding="utf-8")
    assert "ClearAscensionTalentState(player)" in change and '"core.ascension_build."' not in change, (
        "the class change clears the stored advancement state by setting name again: the name it "
        "used to clear is not the one the build and the loadouts live in")
    with tempfile.TemporaryDirectory(prefix="coa-talent-state-") as directory:
        out = Path(directory)
        for name, text in STUBS.items():
            (out / name).write_text(text, encoding="utf-8")
        (out / "main.cpp").write_text(MAIN, encoding="utf-8")
        includes = [out, ROOT / "src/server/coa", ROOT / "src/server/shared/DataStores",
                    ROOT / "src/common"]
        sources = [out / "main.cpp", ROOT / "src/server/coa/AscensionCoATalentData.cpp",
                   ROOT / "src/server/coa/AscensionCoATalentState.cpp",
                   ROOT / "src/server/shared/DataStores/ClientDBC.cpp"]
        executable = out / ("state.exe" if os.name == "nt" else "state")
        if Path(compiler).stem.lower() == "cl":
            flags = ["/nologo", "/std:c++20", "/EHsc", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
                     *["/I" + str(p) for p in includes], *map(str, sources), "/Fe" + str(executable)]
        else:
            flags = ["-std=c++20", "-Wall", "-Wextra", *["-I" + str(p) for p in includes], *map(str, sources),
                     "-o", str(executable)]
        build = subprocess.run([compiler, *flags], cwd=out, capture_output=True, text=True, errors="replace")
        if build.returncode:
            raise SystemExit("Talent state harness did not compile:\n" + build.stdout + build.stderr)
        result = subprocess.run([str(executable), str(args.dbc_dir.resolve())], text=True)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
