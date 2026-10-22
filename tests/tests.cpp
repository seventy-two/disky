// Tests for the client's portable code. Build and run with build.bat, or on
// Linux with the command in the README.
#include <cstdio>
#include <string>

#include "../src/markdown.h"
#include "../src/store.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        auto va = (a);                                                                     \
        auto vb = (b);                                                                     \
        if (!(va == vb)) {                                                                 \
            std::printf("%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

using namespace disky;

static void testTime() {
    CHECK_EQ(ParseTime("1970-01-01T00:00:00+00:00"), int64_t(0));
    CHECK_EQ(ParseTime("2026-10-22T17:53:51.123000+00:00"), int64_t(1792691631123));
    CHECK_EQ(ParseTime("2026-10-22T18:53:51.123+01:00"), int64_t(1792691631123));
    CHECK_EQ(ParseTime("nope"), int64_t(0));
    // 175928847299117063 is Discord's documented example: 2016-04-30 11:18:25.796 UTC.
    CHECK_EQ(IdTime(175928847299117063ull), int64_t(1462015105796));
}

static void testBase64AndProto() {
    CHECK_EQ(Base64Encode("hello"), std::string("aGVsbG8="));
    CHECK_EQ(Base64Decode("aGVsbG8="), std::string("hello"));
    CHECK_EQ(Base64Decode(Base64Encode(std::string("\x00\xff\x10", 3))), std::string("\x00\xff\x10", 3));

    // guild_folders (14) { folders (1) { guild_ids (1, packed fixed64): 5, 3 }, folders { guild_ids: 9 } }
    auto fixed = [](uint64_t v) {
        std::string s(8, '\0');
        for (int i = 0; i < 8; i++) s[i] = char((v >> (8 * i)) & 0xFF);
        return s;
    };
    auto field = [](int num, const std::string& body) {
        std::string s;
        s += char((num << 3) | 2);
        s += char(body.size());
        return s + body;
    };
    std::string folder1 = field(1, fixed(5) + fixed(3)), folder2 = field(1, fixed(9));
    std::string folders = field(1, folder1) + field(1, folder2);
    // Field 14's key needs two bytes as a varint: (14 << 3) | 2 = 114.
    std::string proto = std::string("\x08\x01", 2) + char(114) + char(folders.size()) + folders;
    auto order = ReadProtoGuildOrder(proto);
    CHECK_EQ(order.size(), size_t(3));
    if (order.size() == 3) {
        CHECK_EQ(order[0], Snowflake(5));
        CHECK_EQ(order[1], Snowflake(3));
        CHECK_EQ(order[2], Snowflake(9));
    }
}

static json readyFixture() {
    return json::parse(R"({
      "user": {"id": "1", "username": "me"},
      "users": [{"id": "2", "username": "friend", "global_name": "Friend"}],
      "guilds": [{
        "id": "100",
        "properties": {"name": "Guild", "owner_id": "9"},
        "roles": [
          {"id": "100", "name": "@everyone", "permissions": "1024", "position": 0, "color": 0},
          {"id": "101", "name": "Mods", "permissions": "0", "position": 2, "color": 16711680, "hoist": true},
          {"id": "102", "name": "Blue", "permissions": "0", "position": 1, "color": 255}
        ],
        "channels": [
          {"id": "200", "type": 4, "name": "Cat", "position": 0},
          {"id": "201", "type": 0, "name": "general", "position": 0, "parent_id": "200", "last_message_id": "500"},
          {"id": "202", "type": 0, "name": "secret", "position": 1, "parent_id": "200",
           "permission_overwrites": [{"id": "100", "type": 0, "allow": "0", "deny": "1024"}]},
          {"id": "203", "type": 0, "name": "loose", "position": 5},
          {"id": "204", "type": 2, "name": "voice", "position": 0, "parent_id": "200"},
          {"id": "205", "type": 4, "name": "Empty", "position": 1}
        ],
        "emojis": [{"id": "300", "name": "pog"}]
      }],
      "merged_members": [[{"user_id": "1", "roles": ["102"]}]],
      "private_channels": [
        {"id": "400", "type": 1, "recipient_ids": ["2"], "last_message_id": "600"},
        {"id": "401", "type": 3, "recipient_ids": ["2"], "name": "", "last_message_id": "650"}
      ],
      "read_state": {"entries": [
        {"id": "201", "last_message_id": "400", "mention_count": 0},
        {"id": "400", "last_message_id": "600", "mention_count": 0}
      ]},
      "user_guild_settings": {"entries": [{"guild_id": "999", "muted": true}]}
    })");
}

static void testReady() {
    Store s;
    auto fx = s.Apply("READY", readyFixture());
    CHECK_EQ(fx.size(), size_t(1));
    CHECK_EQ(s.me, Snowflake(1));
    CHECK_EQ(s.guilds.at(100).name, std::string("Guild"));
    // Settings for a guild we've left don't make it appear.
    CHECK_EQ(s.guilds.size(), size_t(1));
    // general is visible and in its category, secret is denied, voice isn't
    // text, and the empty category is left out.
    auto list = s.GuildChannels(100);
    CHECK_EQ(list.size(), size_t(3));
    if (list.size() == 3) {
        CHECK_EQ(list[0], Snowflake(203));
        CHECK_EQ(list[1], Snowflake(200));
        CHECK_EQ(list[2], Snowflake(201));
    }
    CHECK(!s.CanView(202));
    CHECK(s.Unread(201));
    CHECK(!s.Unread(400));
    CHECK(s.GuildUnread(100));
    CHECK_EQ(s.ChannelName(400), std::string("Friend"));
    CHECK_EQ(s.ChannelName(401), std::string("Friend"));
    auto dms = s.PrivateChannels();
    CHECK_EQ(dms.size(), size_t(2));
    if (dms.size() == 2) CHECK_EQ(dms[0], Snowflake(401));
    CHECK_EQ(s.Colour(100, 1), uint32_t(255));
}

static void testPermissions() {
    Store s;
    s.Apply("READY", readyFixture());
    // An allow for one of our roles beats @everyone's deny.
    s.channels[202].overwrites.push_back({102, 0, 1024, 0});
    CHECK(s.CanView(202));
    // A member overwrite beats roles.
    s.channels[202].overwrites.push_back({1, 1, 0, 1024});
    CHECK(!s.CanView(202));
    // The owner sees everything.
    s.guilds[100].ownerId = 1;
    CHECK(s.CanView(202));
}

static json message(const char* id, const char* channel, const char* author, const char* content) {
    return {{"id", id},
            {"channel_id", channel},
            {"author", {{"id", author}, {"username", std::string("u") + author}}},
            {"content", content},
            {"timestamp", "2026-10-22T10:00:00+00:00"}};
}

static void testMessages() {
    Store s;
    s.Apply("READY", readyFixture());
    // Live messages before the channel's loaded aren't kept, but they do
    // move its last message on.
    s.Apply("MESSAGE_CREATE", message("700", "201", "2", "hi"));
    CHECK(s.messages[201].list.empty());
    CHECK_EQ(s.channels[201].lastMessageId, Snowflake(700));

    json page = json::array({message("700", "201", "2", "hi"), message("690", "201", "2", "earlier")});
    s.Page(201, page, 0, 50);
    CHECK(s.messages[201].loaded);
    CHECK(s.messages[201].reachedStart);
    CHECK_EQ(s.messages[201].list.size(), size_t(2));
    CHECK_EQ(s.messages[201].list.front().id, Snowflake(690));

    // Our message shows as pending, then the server's copy replaces it.
    Message pending;
    pending.id = 800;
    pending.channelId = 201;
    pending.authorId = 1;
    pending.content = "mine";
    pending.nonce = "800";
    s.AddPending(pending);
    s.Apply("MESSAGE_CREATE", message("710", "201", "2", "between"));
    CHECK_EQ(s.messages[201].list.back().content, std::string("mine"));
    json mine = message("720", "201", "1", "mine");
    mine["nonce"] = "800";
    s.Apply("MESSAGE_CREATE", mine);
    CHECK_EQ(s.messages[201].list.size(), size_t(4));
    CHECK(!s.messages[201].list.back().pending);
    CHECK_EQ(s.messages[201].list.back().id, Snowflake(720));
    // Sending marks the channel read.
    CHECK(!s.Unread(201));

    s.Apply("MESSAGE_UPDATE", {{"id", "710"}, {"channel_id", "201"}, {"content", "edited"},
                               {"edited_timestamp", "2026-10-22T10:01:00+00:00"}});
    CHECK_EQ(s.messages[201].list[2].content, std::string("edited"));
    CHECK(s.messages[201].list[2].edited > 0);

    s.Apply("MESSAGE_REACTION_ADD", {{"channel_id", "201"}, {"message_id", "710"}, {"user_id", "2"},
                                     {"emoji", {{"id", nullptr}, {"name", "👍"}}}});
    s.Apply("MESSAGE_REACTION_ADD", {{"channel_id", "201"}, {"message_id", "710"}, {"user_id", "1"},
                                     {"emoji", {{"id", nullptr}, {"name", "👍"}}}});
    auto& r = s.messages[201].list[2].reactions;
    CHECK_EQ(r.size(), size_t(1));
    if (!r.empty()) {
        CHECK_EQ(r[0].count, 2);
        CHECK(r[0].me);
    }
    s.Apply("MESSAGE_REACTION_REMOVE", {{"channel_id", "201"}, {"message_id", "710"}, {"user_id", "2"},
                                        {"emoji", {{"id", nullptr}, {"name", "👍"}}}});
    s.Apply("MESSAGE_REACTION_REMOVE", {{"channel_id", "201"}, {"message_id", "710"}, {"user_id", "1"},
                                        {"emoji", {{"id", nullptr}, {"name", "👍"}}}});
    CHECK(s.messages[201].list[2].reactions.empty());

    s.Apply("MESSAGE_DELETE", {{"id", "690"}, {"channel_id", "201"}});
    CHECK_EQ(s.messages[201].list.size(), size_t(3));
}

static void testMentions() {
    Store s;
    s.Apply("READY", readyFixture());
    json m = message("900", "201", "2", "<@1> hello");
    m["mentions"] = json::array({{{"id", "1"}, {"username", "me"}}});
    auto fx = s.Apply("MESSAGE_CREATE", m);
    CHECK_EQ(fx.size(), size_t(1));
    if (!fx.empty()) CHECK(fx[0].type == Effect::Type::Mention);
    CHECK_EQ(s.Mentions(201), 1);

    json role = message("901", "201", "2", "<@&102>");
    role["mention_roles"] = json::array({"102"});
    CHECK_EQ(s.Apply("MESSAGE_CREATE", role).size(), size_t(1));
    json other = message("902", "201", "2", "<@&101>");
    other["mention_roles"] = json::array({"101"});
    CHECK(s.Apply("MESSAGE_CREATE", other).empty());

    // DMs always notify; muting the channel stops it.
    CHECK_EQ(s.Apply("MESSAGE_CREATE", message("903", "400", "2", "dm")).size(), size_t(1));
    s.Apply("USER_GUILD_SETTINGS_UPDATE", {{"guild_id", nullptr},
                                           {"channel_overrides", json::array({{{"channel_id", "400"}, {"muted", true}}})}});
    CHECK(s.Apply("MESSAGE_CREATE", message("904", "400", "2", "dm")).empty());

    // @everyone, unless suppressed.
    json every = message("905", "201", "2", "@everyone");
    every["mention_everyone"] = true;
    CHECK_EQ(s.Apply("MESSAGE_CREATE", every).size(), size_t(1));
    s.Apply("USER_GUILD_SETTINGS_UPDATE", {{"guild_id", "100"}, {"suppress_everyone", true}});
    every["id"] = "906";
    CHECK(s.Apply("MESSAGE_CREATE", every).empty());
}

static void testMemberList() {
    Store s;
    s.Apply("READY", readyFixture());
    s.Apply("GUILD_MEMBER_LIST_UPDATE", json::parse(R"({
      "guild_id": "100", "id": "everyone", "member_count": 3, "online_count": 2,
      "ops": [{"op": "SYNC", "range": [0, 99], "items": [
        {"group": {"id": "101", "count": 1}},
        {"member": {"user": {"id": "5", "username": "mod"}, "roles": ["101"], "presence": {"status": "online"}}},
        {"group": {"id": "online", "count": 1}},
        {"member": {"user": {"id": "2", "username": "friend"}, "roles": [], "presence": {"status": "idle"}}}
      ]}]
    })"));
    const MemberList& l = s.guilds[100].lists["everyone"];
    CHECK_EQ(l.items.size(), size_t(100));
    CHECK_EQ(l.items[0].group, std::string("101"));
    CHECK_EQ(l.items[1].userId, Snowflake(5));
    CHECK_EQ(s.Colour(100, 5), uint32_t(16711680));
    s.Apply("GUILD_MEMBER_LIST_UPDATE", json::parse(R"({
      "guild_id": "100", "id": "everyone",
      "ops": [{"op": "DELETE", "index": 1}, {"op": "INSERT", "index": 3,
               "item": {"member": {"user": {"id": "6", "username": "new"}, "roles": []}}}]
    })"));
    CHECK_EQ(s.guilds[100].lists["everyone"].items[1].group, std::string("online"));
    CHECK_EQ(s.guilds[100].lists["everyone"].items[3].userId, Snowflake(6));
}

static std::string kinds(const std::vector<Para>& ps) {
    std::string out;
    for (const auto& p : ps) {
        if (!out.empty()) out += "|";
        if (p.code) {
            out += "code(" + p.lang + ":" + p.text + ")";
            continue;
        }
        for (const auto& s : p.spans) {
            switch (s.kind) {
            case Span::Kind::Text: out += "t"; break;
            case Span::Kind::Emoji: out += "e"; break;
            case Span::Kind::CustomEmoji: out += "c"; break;
            case Span::Kind::UserMention: out += "@"; break;
            case Span::Kind::RoleMention: out += "&"; break;
            case Span::Kind::ChannelMention: out += "#"; break;
            case Span::Kind::Everyone: out += "!"; break;
            case Span::Kind::Command: out += "/"; break;
            case Span::Kind::Link: out += "l"; break;
            case Span::Kind::Timestamp: out += "T"; break;
            }
            if (s.style) out += std::to_string(s.style);
            out += "[" + s.text + "]";
        }
    }
    return out;
}

static void testMarkdown() {
    CHECK_EQ(kinds(Parse("plain text")), std::string("t[plain text]"));
    CHECK_EQ(kinds(Parse("a **bold** b")), std::string("t[a ]t1[bold]t[ b]"));
    CHECK_EQ(kinds(Parse("***both***")), std::string("t3[both]"));
    CHECK_EQ(kinds(Parse("*it* _also_ snake_case_name")), std::string("t2[it]t[ ]t2[also]t[ snake_case_name]"));
    CHECK_EQ(kinds(Parse("__u__ ~~s~~")), std::string("t4[u]t[ ]t8[s]"));
    CHECK_EQ(kinds(Parse("`**not bold**`")), std::string("t16[**not bold**]"));
    CHECK_EQ(kinds(Parse("\\*not\\*")), std::string("t[*not*]"));
    CHECK_EQ(kinds(Parse("2 * 3 * 4")), std::string("t[2 * 3 * 4]"));
    CHECK_EQ(kinds(Parse("hi <@123> <@!4> <@&5> <#6>")), std::string("t[hi ]@[]t[ ]@[]t[ ]&[]t[ ]#[]"));
    CHECK_EQ(kinds(Parse("<:pog:300><a:dance:301>")), std::string("c[pog]c[dance]"));
    CHECK_EQ(kinds(Parse("see https://example.com/a_(b). ok")),
             std::string("t[see ]l[https://example.com/a_(b)]t[. ok]"));
    CHECK_EQ(kinds(Parse("[label](https://x.com) <https://y.com>")), std::string("l[label]t[ ]l[https://y.com]"));
    CHECK_EQ(kinds(Parse("<t:1700000000:R>")), std::string("T[]"));
    CHECK_EQ(kinds(Parse("@everyone look")), std::string("![@everyone]t[ look]"));
    CHECK_EQ(kinds(Parse("||spoiler||")), std::string("t32[spoiler]"));
    CHECK_EQ(kinds(Parse("a\n```js\nx = 1\n```\nb")), std::string("t[a]|code(js:x = 1)|t[b]"));
    CHECK_EQ(kinds(Parse("```inline```")), std::string("code(:inline)"));

    auto ps = Parse("> quoted\n# Big\n-# small\n- item\n  - nested\n3. third");
    CHECK_EQ(ps.size(), size_t(6));
    if (ps.size() == 6) {
        CHECK(ps[0].quote);
        CHECK_EQ(ps[1].heading, 1);
        CHECK(ps[2].subtext);
        CHECK_EQ(ps[3].list, 1);
        CHECK_EQ(ps[4].list, 2);
        CHECK_EQ(ps[5].marker, std::string("3."));
    }
    auto q = Parse(">>> all\nof this");
    CHECK(q.size() == 2 && q[0].quote && q[1].quote);

    bool jumbo = false;
    Parse("😀 <:pog:1>", &jumbo);
    CHECK(jumbo);
    Parse("😀 hi", &jumbo);
    CHECK(!jumbo);
}

static void testEmoji() {
    std::string file;
    CHECK_EQ(EmojiAt("😀", 0, &file), size_t(4));
    CHECK_EQ(file, std::string("1f600"));
    // Heart with U+FE0F drops it in the file name.
    CHECK_EQ(EmojiAt("\xE2\x9D\xA4\xEF\xB8\x8F", 0, &file), size_t(6));
    CHECK_EQ(file, std::string("2764"));
    // A bare heart is text.
    CHECK_EQ(EmojiAt("\xE2\x9D\xA4", 0), size_t(0));
    // ⚡ is an emoji by default.
    CHECK_EQ(EmojiAt("\xE2\x9A\xA1", 0), size_t(3));
    // Thumbs up, medium skin tone.
    CHECK_EQ(EmojiAt("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 0, &file), size_t(8));
    CHECK_EQ(file, std::string("1f44d-1f3fd"));
    // Man technologist: a ZWJ sequence.
    CHECK_EQ(EmojiAt("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB", 0, &file), size_t(11));
    CHECK_EQ(file, std::string("1f468-200d-1f4bb"));
    // UK flag.
    CHECK_EQ(EmojiAt("\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7", 0, &file), size_t(8));
    CHECK_EQ(file, std::string("1f1ec-1f1e7"));
    // Keycap one.
    CHECK_EQ(EmojiAt("1\xEF\xB8\x8F\xE2\x83\xA3", 0, &file), size_t(7));
    CHECK_EQ(file, std::string("31-20e3"));
    CHECK_EQ(EmojiAt("1", 0), size_t(0));
    CHECK_EQ(EmojiAt("a", 0), size_t(0));

    const EmojiEntry* e = EmojiByName("thumbsup");
    CHECK(e != nullptr);
    if (e) CHECK_EQ(std::string(e->emoji), std::string("\xF0\x9F\x91\x8D"));
    CHECK(EmojiByName("+1") != nullptr);
    CHECK(EmojiByName("not_an_emoji") == nullptr);
}

int main() {
    testTime();
    testBase64AndProto();
    testReady();
    testPermissions();
    testMessages();
    testMentions();
    testMemberList();
    testMarkdown();
    testEmoji();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("All tests passed\n");
    return 0;
}
