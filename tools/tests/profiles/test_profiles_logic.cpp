// Host-side tests for profiles_logic.h (no engine, no windows.h).
#include "profiles_logic.h"

#include <cassert>
#include <iostream>
#include <string>

static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

int main() {
    using namespace profiles;

    // GuidValid: 24-digit ok, 62-digit legacy ok, else reject.
    CHECK(GuidValid("602380633711624767525303"));
    CHECK(!GuidValid("60238063371162476752530"));   // 23
    CHECK(!GuidValid("6023806337116247675253031")); // 25
    CHECK(!GuidValid("60238063371162476752530a"));  // non-digit
    CHECK(!GuidValid(""));
    CHECK(!GuidValid("0"));
    CHECK(!GuidValid("1"));
    std::string legacy(62, '7');
    CHECK(GuidValid(legacy));
    CHECK(!GuidValid(std::string(61, '7')));
    CHECK(!GuidValid(std::string(63, '7')));

    // NicknameClean: colors stripped, lowercased, a-z0-9 kept.
    CHECK(NicknameClean("Slot0Test") == "slot0test");
    CHECK(NicknameClean("%02Player%04") == "player");
    CHECK(NicknameClean("^1Red^3Wolf") == "redwolf");
    CHECK(NicknameClean("A B-C_D!E") == "abcde");
    CHECK(NicknameClean("---") == "");
    CHECK(NicknameClean("") == "");
    CHECK(NicknameClean("%02") == "");
    CHECK(NicknameClean("abc123") == "abc123");

    {
        // Prefix text blue-/red- is unworkable (AssignTeam atoi → 0) and
        // not accepted.
        TrbParsed p = ParseTeamRedBlue("blue-602380633711624767525303-0");
        CHECK(!p.valid && p.team == -1);
        p = ParseTeamRedBlue("red-602380633711624767525303-1");
        CHECK(!p.valid && p.team == -1);
    }
    {
        // Plain all-digit <guid><bit> (legacy emit / read).
        TrbParsed p = ParseTeamRedBlue("6023806337116247675253030");
        CHECK(p.valid && p.team == 0 && p.identity == "602380633711624767525303");
    }
    {
        TrbParsed p = ParseTeamRedBlue(std::string(62, '7') + "1");
        CHECK(p.valid && p.team == 1 && p.identity == std::string(62, '7'));
    }
    {
        // Collapsed bare team bit: no identity, team known.
        TrbParsed p = ParseTeamRedBlue("0");
        CHECK(!p.valid && p.team == 0 && p.identity.empty());
        p = ParseTeamRedBlue("1");
        CHECK(!p.valid && p.team == 1 && p.identity.empty());
    }
    {
        TrbParsed p = ParseTeamRedBlue("");
        CHECK(!p.valid && p.team == -1);
        p = ParseTeamRedBlue("garbage!");
        CHECK(!p.valid && p.team == -1);
    }
    {
        // Short digit string is not a guid: team only.
        TrbParsed p = ParseTeamRedBlue("123");
        CHECK(!p.valid && p.team == 1);
    }

    // TeamRedBlueAllowsAssignTeam: guid shapes ok; collapsed/garbage blocked.
    {
        CHECK(TeamRedBlueAllowsAssignTeam(""));
        CHECK(TeamRedBlueAllowsAssignTeam("6023806337116247675253030-blue"));
        CHECK(TeamRedBlueAllowsAssignTeam("6023806337116247675253030"));
        CHECK(!TeamRedBlueAllowsAssignTeam("0"));
        CHECK(!TeamRedBlueAllowsAssignTeam("1"));
        CHECK(!TeamRedBlueAllowsAssignTeam("garbage"));
    }
    // SoF atol wraps <guid><bit> to 32 bits. 303041696 is norway's blue push.
    {
        CHECK(AtolWrapDecimal("5881536207746508577049760") == "303041696");
        int bit = -1;
        CHECK(WrapMatchesGuid("303041696", "588153620774650857704976", &bit) && bit == 0);
        CHECK(WrapMatchesGuid("303041697", "588153620774650857704976", &bit) && bit == 1);
        CHECK(!WrapMatchesGuid("303041696", "051399832444306953452386", &bit));
        CHECK(!WrapMatchesGuid("0", "588153620774650857704976", &bit));
        CHECK(TeamRedBlueIsBareBit("0") && TeamRedBlueIsBareBit("1"));
        CHECK(!TeamRedBlueIsBareBit("303041696"));
        // CVAR_INT then stores that atol as float32. Both norway bits collapse.
        const char* norway = "588153620774650857704976";
        const char* dende = "051399832444306953452386";
        CHECK(CvarIntDecimal(std::string(norway) + "0") == "303041696");
        CHECK(CvarIntDecimal(std::string(norway) + "1") == "303041696");
        CHECK(IntCvarTruncation("303041696", norway, &bit) && bit == -1);
        // dende/acadie: float32 rounds away from the 32-bit atol.
        const char* acadie = "153404870843790625472604";
        CHECK(AtolWrapDecimal(std::string(dende) + "0") == "1676235220");
        CHECK(CvarIntDecimal(std::string(dende) + "0") == "1676235264");
        CHECK(CvarIntDecimal(std::string(dende) + "1") == "1676235264");
        CHECK(IntCvarTruncation("1676235264", dende, &bit) && bit == -1);
        CHECK(!IntCvarTruncation("1676235220", dende, &bit));
        CHECK(CvarIntDecimal(std::string(acadie) + "0") == "423363488");
        CHECK(CvarIntDecimal(std::string(acadie) + "1") == "423363488");
        CHECK(IntCvarTruncation("423363488", acadie, &bit) && bit == -1);
        std::map<std::string, std::string> roster = {
            {dende, "dende"}, {acadie, "acadie"}, {norway, "norway"}};
        std::string who;
        CHECK(MatchRosterTruncation("1676235264", roster, &who, &bit) &&
              who == dende && bit == -1);
        CHECK(MatchRosterTruncation("423363488", roster, &who, &bit) && who == acadie);
        CHECK(MatchRosterTruncation("303041696", roster, &who, &bit) && who == norway);
        CHECK(!MatchRosterTruncation("1676235220", roster, &who, &bit));
        CHECK(!IntCvarMatchesGuid("303041696", norway, &bit));
        std::string red = IntCvarPackDecimal(norway, 1);
        std::string blue = IntCvarPackDecimal(norway, 0);
        CHECK(red != blue);
        CHECK(IntCvarPack(norway, 0) < (1u << 24) && IntCvarPack(norway, 1) < (1u << 24));
        CHECK(CvarIntStored(static_cast<std::int32_t>(IntCvarPack(norway, 1))) ==
              static_cast<std::int32_t>(IntCvarPack(norway, 1)));
        CHECK((IntCvarPack(norway, 1) & 1u) == 1u && (IntCvarPack(norway, 0) & 1u) == 0u);
        std::uint32_t fold = 0;
        CHECK(IntCvarSplit(red, &fold, &bit) && bit == 1 && fold == GuidFold23(norway));
        CHECK(IntCvarMatchesGuid(red, norway, &bit) && bit == 1);
        CHECK(!IntCvarMatchesGuid(red, dende, &bit));
        CHECK(GuidFold23(norway) != GuidFold23(dende));
        CHECK(TeamRedBlueForClient(norway, 1, true) == red);
        CHECK(TeamRedBlueForClient(norway, 0, false) == std::string(norway) + "0-blue");
        CHECK(IntCvarTeam("303041696", norway, 1, 0) == 1);
        CHECK(IntCvarTeam(red, norway, 0, 0) == 1);
    }
    // ResolveRestoreTeam: userinfo bit wins over live server team.
    {
        CHECK(ResolveRestoreTeam(0, 1, -1) == 1);
        CHECK(ResolveRestoreTeam(0, -1, 1) == 0);
        CHECK(ResolveRestoreTeam(-1, -1, 1) == 1);
        CHECK(ResolveRestoreTeam(-1, -1, -1) == -1);
    }

    // BuildTeamRedBlue emits <guid><bit>-color: the bit terminates the
    // leading digit run so AssignTeam's atoi() reads it. Round-trips
    // through the parser.
    {
        std::string full = BuildTeamRedBlue("602380633711624767525303", 0);
        CHECK(full == "6023806337116247675253030-blue");
        TrbParsed p = ParseTeamRedBlue(full);
        CHECK(p.valid && p.team == 0 && p.identity == "602380633711624767525303");
        full = BuildTeamRedBlue("602380633711624767525303", 1);
        CHECK(full == "6023806337116247675253031-red");
        p = ParseTeamRedBlue(full);
        CHECK(p.valid && p.team == 1);
        // Odd team values fold to the low bit.
        CHECK(BuildTeamRedBlue("602380633711624767525303", 3) ==
              "6023806337116247675253031-red");
        // Legacy 62-digit guids stay plain all-digit: with suffix they'd
        // exceed the engine's 64-char userinfo value limit.
        std::string legacy(62, '7');
        CHECK(BuildTeamRedBlue(legacy, 0) == legacy + "0");
        CHECK(BuildTeamRedBlue(legacy, 1) == legacy + "1");
    }
    // Suffixed form parse: valid, mismatch (team kept, identity dropped),
    // and wrong-length digit runs rejected.
    {
        TrbParsed p = ParseTeamRedBlue("6023806337116247675253030-blue");
        CHECK(p.valid && p.team == 0 && p.identity == "602380633711624767525303");
        p = ParseTeamRedBlue("6023806337116247675253031-RED");
        CHECK(p.valid && p.team == 1 && p.identity == "602380633711624767525303");
        p = ParseTeamRedBlue("6023806337116247675253030-red");
        CHECK(!p.valid && p.team == 0 && p.identity.empty());
        p = ParseTeamRedBlue("1230-blue");
        CHECK(!p.valid && p.team == 0);
    }

    // Registry: add / lookup / reverse lookup / remove.
    {
        Registry r;
        std::string err;
        CHECK(r.Add("602380633711624767525303", "Slot0Test", &err));
        CHECK(r.Size() == 1);
        std::string nick;
        CHECK(r.Lookup("602380633711624767525303", &nick) && nick == "Slot0Test");
        std::string guid;
        CHECK(r.FindByNickname("Slot0Test", &guid) && guid == "602380633711624767525303");
        CHECK(r.FindByNickname("slot0test", &guid) && guid == "602380633711624767525303");
        CHECK(r.FindByClean("slot0test", &guid));
        CHECK(!r.Lookup("000000000000000000000000", nullptr));
        // Bad adds fail with an error.
        CHECK(!r.Add("short", "Nick", &err) && !err.empty());
        CHECK(!r.Add("602380633711624767525303", "", &err));
        CHECK(!r.Add("602380633711624767525303", "---", &err));
        // Remove by guid.
        CHECK(r.Remove("602380633711624767525303", "", &err));
        CHECK(r.Size() == 0);
        CHECK(!r.FindByNickname("Slot0Test", nullptr));
        // Remove by nickname.
        CHECK(r.Add("111111111111111111111111", "Alice", nullptr));
        CHECK(r.Add("222222222222222222222222", "Bob", nullptr));
        CHECK(r.Remove("", "alice", &err));
        CHECK(r.Size() == 1);
        CHECK(!r.Remove("", "nobody", &err));
    }

    // Registry save/load round-trip (native format).
    {
        Registry r;
        CHECK(r.Add("602380633711624767525303", "Slot0 Test", nullptr));
        CHECK(r.Add("111111111111111111111111", "Alice", nullptr));
        std::string text = r.Save();
        Registry r2;
        CHECK(r2.Load(text) == 2);
        std::string nick;
        CHECK(r2.Lookup("602380633711624767525303", &nick) && nick == "Slot0 Test");
        CHECK(r2.FindByNickname("alice", nullptr));
    }

    // Legacy profiles.func registry.cfg lines import.
    {
        Registry r;
        std::string legacyFile =
            "set \"~reg_602380633711624767525303\" \"Slot0Test\"\n"
            "set \"~guid_by_slot0test\" \"602380633711624767525303\"\n"
            "set '~reg_111111111111111111111111' 'Alice'\n";
        CHECK(r.Load(legacyFile) == 2);
        std::string nick;
        CHECK(r.Lookup("602380633711624767525303", &nick) && nick == "Slot0Test");
        CHECK(r.Lookup("111111111111111111111111", &nick) && nick == "Alice");
        std::string guid;
        CHECK(r.FindByNickname("SLOT0TEST", &guid));
    }

    // MintIdentity: 24 digits, valid guid, varies with entropy.
    {
        unsigned counter = 0;
        std::string m1 = MintIdentity([&]() -> unsigned { return counter++; });
        CHECK(m1.size() == 24 && GuidValid(m1));
        CHECK(m1 == "012345678901234567890123");
        // Byte-valued entropy (engine RtlGenRandom path) folds via mod 10.
        unsigned char bytes[24] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                   13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
        std::size_t i = 0;
        std::string m2 = MintIdentity([&]() -> unsigned { return bytes[i++ % 24]; });
        CHECK(m2.size() == 24 && GuidValid(m2));
        CHECK(m2 == "012345678901234567890123");
        // Two mints from a stepping source differ (collision retry matters).
        unsigned seed = 7;
        auto lcg = [&]() -> unsigned {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 16) & 0xFFu;
        };
        std::string a = MintIdentity(lcg);
        std::string b = MintIdentity(lcg);
        CHECK(GuidValid(a) && GuidValid(b) && a != b);
        // Minted guid is immediately usable as a registry key.
        Registry r;
        CHECK(r.Add(a, "Minted", nullptr));
        CHECK(!r.Lookup(b, nullptr) || a != b);
    }

    // SlotState defaults (anchor model).
    {
        SlotState st;
        CHECK(st.guid.empty());
        CHECK(st.team == -1 && !st.registered && !st.offered);
    }

    std::cout << "profiles_logic: all " << g_checks << " checks passed\n";
    return 0;
}
