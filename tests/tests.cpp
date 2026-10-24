// Tests for the client's portable code. Build and run with build.bat, or on
// Linux with the command in the README.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

#include "../src/commands.h"
#include "../src/gif.h"
#include "../src/h264.h"
#include "../src/markdown.h"
#include "../src/rtp.h"
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
        "emojis": [{"id": "300", "name": "pog"}],
        "voice_states": [{"user_id": "2", "channel_id": "204", "self_mute": true}]
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

static void testVoice() {
    Store s;
    s.Apply("READY", readyFixture());
    // The voice channel, in its category; text channels left out.
    auto list = s.VoiceChannels(100);
    CHECK_EQ(list.size(), size_t(2));
    if (list.size() == 2) {
        CHECK_EQ(list[0], Snowflake(200));
        CHECK_EQ(list[1], Snowflake(204));
    }
    CHECK_EQ(s.VoiceCount(100), 1);
    auto in = s.InVoice(204);
    CHECK_EQ(in.size(), size_t(1));
    if (!in.empty()) CHECK(in[0].selfMute && !in[0].streaming);
    // Someone joins and goes live; the friend leaves.
    s.Apply("VOICE_STATE_UPDATE", json::parse(R"({"guild_id": "100", "channel_id": "204", "user_id": "3",
        "self_stream": true, "member": {"user": {"id": "3", "username": "zed"}, "roles": []}})"));
    s.Apply("VOICE_STATE_UPDATE", json::parse(R"({"guild_id": "100", "channel_id": null, "user_id": "2"})"));
    in = s.InVoice(204);
    CHECK_EQ(in.size(), size_t(1));
    if (!in.empty()) CHECK(in[0].userId == 3 && in[0].streaming);
    CHECK_EQ(s.DisplayName(100, 3), std::string("zed"));
    s.Apply("VOICE_STATE_UPDATE", json::parse(R"({"guild_id": "100", "channel_id": null, "user_id": "3"})"));
    CHECK_EQ(s.VoiceCount(100), 0);
    // A user account gets who's in voice with READY_SUPPLEMENTAL.
    s.Apply("READY_SUPPLEMENTAL", json::parse(R"({"guilds": [{"id": "100", "voice_states": [
        {"user_id": "2", "channel_id": "204", "self_deaf": true, "self_video": true}]}]})"));
    in = s.InVoice(204);
    CHECK(in.size() == 1 && in[0].selfDeaf && in[0].video);
}

static void testRtp() {
    // A header with one CSRC and a one-word extension, padding set.
    std::string pk = {char(0xB1), char(0x78), 0x01, 0x02, 0, 0, 0x03, char(0xC0), 0x11, 0x22, 0x33, 0x44};
    pk += std::string(4, 'c');                            // CSRC
    pk += std::string{char(0xBE), char(0xDE), 0, 1};      // extension header: 1 word
    pk += "body";                                         // ciphertext from here
    std::string tagNonce = std::string(16, 't') + std::string{0, 0, 0, 7};
    pk += tagNonce;
    Rtp r;
    CHECK(ParseRtp(reinterpret_cast<const uint8_t*>(pk.data()), pk.size(), &r));
    CHECK_EQ(r.seq, uint16_t(0x0102));
    CHECK_EQ(r.timestamp, uint32_t(960));
    CHECK_EQ(r.ssrc, uint32_t(0x11223344));
    CHECK_EQ(r.payloadType, uint8_t(0x78));
    CHECK_EQ(r.header, size_t(20));
    CHECK_EQ(r.extensionBytes, size_t(4));
    Sealed s;
    CHECK(SplitSealed(r, pk.size(), reinterpret_cast<const uint8_t*>(pk.data()), &s));
    CHECK_EQ(s.cipher, size_t(20));
    CHECK_EQ(s.cipherLen, size_t(4));
    CHECK_EQ(int(s.nonce[3]), 7);
    CHECK_EQ(int(s.nonce[4]), 0);
    // Decrypted: the extension body, the audio, then two bytes of padding.
    std::string plain = std::string("EXTN") + "opus" + std::string{0, 2};
    CHECK(Unwrap(r, plain));
    CHECK_EQ(plain, std::string("opus"));
    // Too short to be RTP, or not version 2.
    CHECK(!ParseRtp(reinterpret_cast<const uint8_t*>(pk.data()), 8, &r));
    std::string v1 = pk;
    v1[0] = 0x40;
    CHECK(!ParseRtp(reinterpret_cast<const uint8_t*>(v1.data()), v1.size(), &r));

    std::string h = RtpHeader(5, 960, 42);
    CHECK_EQ(h.size(), size_t(12));
    CHECK(ParseRtp(reinterpret_cast<const uint8_t*>(h.data()), h.size(), &r) && r.seq == 5 && r.ssrc == 42);

    std::string d = DiscoveryRequest(42);
    CHECK_EQ(d.size(), size_t(74));
    std::string answer = d;
    answer[1] = 2;
    std::memcpy(&answer[8], "203.0.113.9", 11);
    answer[72] = char(0xC3);
    answer[73] = 0x50;
    std::string ip;
    uint16_t port = 0;
    CHECK(ParseDiscovery(reinterpret_cast<const uint8_t*>(answer.data()), answer.size(), &ip, &port));
    CHECK_EQ(ip, std::string("203.0.113.9"));
    CHECK_EQ(port, uint16_t(50000));

    int seq = 0, op = 0;
    std::string payload;
    CHECK(ParseVoiceBinary(std::string{0, 9, 25} + "xs", &seq, &op, &payload));
    CHECK(seq == 9 && op == 25 && payload == "xs");
    CHECK_EQ(VoiceBinary(26, "kp"), std::string{26} + "kp");

    // Playout: holds back 60ms, conceals small gaps, drops late packets.
    Playout p;
    CHECK_EQ(p.Arrived(100), 0);
    CHECK_EQ(p.Arrived(101), 0);
    CHECK_EQ(p.Arrived(104), 2);
    CHECK_EQ(p.Arrived(103), -1);
    CHECK_EQ(p.Arrived(104), -1);
    CHECK_EQ(p.Arrived(200), 0);  // a pause, not loss
    std::vector<float> frame(kFrameSamples * kChannels, 0.5f), out(kFrameSamples * kChannels, 0);
    p.Push(frame.data(), kFrameSamples);
    CHECK_EQ(p.Mix(out.data(), kFrameSamples, 1), size_t(0));  // still filling
    p.Push(frame.data(), kFrameSamples);
    p.Push(frame.data(), kFrameSamples);
    CHECK_EQ(p.Mix(out.data(), kFrameSamples, 0.5f), size_t(kFrameSamples));
    CHECK(out[0] > 0.24f && out[0] < 0.26f);
    for (int i = 0; i < 20; i++) p.Push(frame.data(), kFrameSamples);
    CHECK(p.Buffered() <= kPlayoutMax);
    // Loudness and the voice activity gate.
    std::vector<float> quiet(960, 0.0f), loud(960, 0.5f);
    CHECK(LevelDb(quiet.data(), quiet.size()) <= -99);
    CHECK(std::fabs(LevelDb(loud.data(), loud.size()) + 6.02f) < 0.1f);
    Gate g;
    CHECK(!g.Frame(-70, -50));
    CHECK(g.Frame(-30, -50));
    int held = 0;
    while (g.Frame(-70, -50)) held++;
    CHECK_EQ(held, kGateHoldFrames - 1);

    Playout w;
    CHECK_EQ(w.Arrived(65535), 0);
    CHECK_EQ(w.Arrived(0), 0);  // wraps
    CHECK_EQ(w.Arrived(2), 1);
}

static void testH264() {
    H264Assembler a;
    std::string out;
    bool lost = false;
    std::string sps = {0x67, 1, 2}, pps = {0x68, 3}, idr = {0x65, 9, 9, 9, 9};
    // A key frame: SPS and PPS in a STAP-A, then the IDR slice in two FU-A pieces.
    std::string stap = {0x78, 0, 3};
    stap += sps;
    stap += std::string{0, 2} + pps;
    std::string fu1 = {0x7C, char(0x85), 9, 9}, fu2 = {0x7C, 0x45, 9, 9};
    CHECK(!a.Packet(10, 3000, false, stap, &out, &lost));
    CHECK(!a.Packet(12, 3000, true, fu2, &out, &lost));  // arrives early
    CHECK(a.Packet(11, 3000, false, fu1, &out, &lost));
    CHECK(!lost);
    std::string s4 = {0, 0, 0, 1};
    CHECK_EQ(out, s4 + sps + s4 + pps + s4 + idr);
    CHECK(H264Assembler::KeyFrame(out));
    // A delta frame in one packet.
    std::string slice = {0x41, 7};
    CHECK(a.Packet(13, 6000, true, slice, &out, &lost));
    CHECK_EQ(out, s4 + slice);
    CHECK(!H264Assembler::KeyFrame(out));
    // Packet 14 is lost: the next frame says so, and deltas wait for a key frame.
    CHECK(!a.Packet(15, 9000, true, slice, &out, &lost));
    CHECK(lost);
    CHECK(!a.Packet(16, 12000, true, slice, &out, &lost));
    CHECK(!lost);
    std::string single = {0x65, 1};
    CHECK(a.Packet(17, 15000, true, single, &out, &lost));
    CHECK_EQ(out, s4 + single);
    // A frame left unfinished when the next starts is lost.
    CHECK(!a.Packet(18, 18000, false, fu1, &out, &lost));
    CHECK(!a.Packet(20, 21000, true, slice, &out, &lost));
    CHECK(lost);
    // Sequence numbers wrapping inside a frame.
    H264Assembler w;
    CHECK(!w.Packet(65535, 100, false, fu1, &out, &lost));
    CHECK(w.Packet(0, 100, true, fu2, &out, &lost));
    CHECK_EQ(out, s4 + idr);
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

    // A bot's answer to a slash command keeps who ran it and what.
    json answer = message("730", "201", "9", "42");
    answer["type"] = 20;
    answer["interaction_metadata"] = {{"id", "1"}, {"type", 2}, {"user", {{"id", "2"}, {"username", "u2"}}}};
    answer["interaction"] = {{"id", "1"}, {"type", 2}, {"name", "weather today"}, {"user", {{"id", "2"}, {"username", "u2"}}}};
    Message cm = s.ParseMessage(answer);
    CHECK(cm.command.has_value());
    if (cm.command) {
        CHECK_EQ(cm.command->userId, Snowflake(2));
        CHECK_EQ(cm.command->name, std::string("weather today"));
    }
    CHECK(!s.ParseMessage(message("731", "201", "2", "plain")).command.has_value());
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
    // A heart with or without U+FE0F is the same emoji.
    const EmojiEntry* heart = EmojiByChar("\xE2\x9D\xA4");
    CHECK(heart != nullptr);
    if (heart) CHECK_EQ(FirstName(*heart), std::string("heart"));
    CHECK(EmojiByChar("\xE2\x9D\xA4\xEF\xB8\x8F") == heart);
}

// px is a w x h patch of one colour; alpha 0 makes it transparent.
static std::vector<unsigned char> px(int w, int h, unsigned char r, unsigned char a = 255) {
    std::vector<unsigned char> v;
    for (int i = 0; i < w * h; i++) v.insert(v.end(), {r, 0, 0, a});
    return v;
}

static void testGif() {
    // Pixel (x, y)'s red on a 2x2 canvas.
    auto red = [](const GifCanvas& c, int x, int y) { return int(c.Pixels()[(y * 2 + x) * 4]); };
    auto alpha = [](const GifCanvas& c, int x, int y) { return int(c.Pixels()[(y * 2 + x) * 4 + 3]); };

    GifCanvas c(2, 2);
    c.Paint({0, 0, 2, 2, 100, 1, px(2, 2, 10)});
    CHECK_EQ(red(c, 1, 1), 10);
    // A patch in one corner, with transparency: the rest shows through.
    auto patch = px(1, 1, 20);
    c.Paint({1, 1, 1, 1, 100, 2, patch});
    CHECK_EQ(red(c, 0, 0), 10);
    CHECK_EQ(red(c, 1, 1), 20);
    // Disposal 2 cleared that corner before the next frame.
    c.Paint({0, 0, 1, 1, 100, 3, px(1, 1, 30, 0)});
    CHECK_EQ(alpha(c, 1, 1), 0);
    CHECK_EQ(red(c, 0, 0), 10);
    // Disposal 3: this frame is undone before the next.
    c.Paint({0, 0, 1, 1, 100, 3, px(1, 1, 40)});
    CHECK_EQ(red(c, 0, 0), 40);
    c.Paint({1, 0, 1, 1, 100, 1, px(1, 1, 50)});
    CHECK_EQ(red(c, 0, 0), 10);
    CHECK_EQ(red(c, 1, 0), 50);
    // Patches running off the canvas are cut short, not written past it.
    c.Paint({1, 1, 3, 3, 100, 1, px(3, 3, 60)});
    CHECK_EQ(red(c, 1, 1), 60);
    CHECK_EQ(c.Pixels().size(), size_t(16));

    CHECK_EQ(GifDelayMs(0), 100);
    CHECK_EQ(GifDelayMs(1), 100);
    CHECK_EQ(GifDelayMs(4), 40);
    CHECK_EQ(GifFrameAt({100, 50, 100}, 0), 0);
    CHECK_EQ(GifFrameAt({100, 50, 100}, 120), 1);
    CHECK_EQ(GifFrameAt({100, 50, 100}, 200), 2);
    CHECK_EQ(GifFrameAt({100, 50, 100}, 260), 0);  // looped
    CHECK_EQ(GifFrameAt({100}, 500), 0);

    CHECK(IsGifUrl("https://media.discordapp.net/attachments/1/2/cat.GIF?width=400&height=300"));
    CHECK(IsGifUrl("https://cdn.discordapp.com/emojis/1.gif?size=48"));
    CHECK(!IsGifUrl("https://cdn.discordapp.com/emojis/1.png?size=48&x=.gif"));
    CHECK(!IsGifUrl("gif"));
}


static void testCommands() {
    json index = json::parse(R"({
      "applications": [{"id": "10", "name": "Rosamund", "icon": "abc"}, {"id": "20", "name": "F1 Bot"}],
      "application_commands": [
        {"id": "1", "type": 1, "application_id": "10", "version": "5", "name": "weather", "description": "Weather",
         "options": [{"type": 3, "name": "units", "description": "Units", "choices": [{"name": "Metric", "value": "mks"}, {"name": "Imperial", "value": "fps"}]},
                     {"type": 3, "name": "location", "description": "Where"}]},
        {"id": "2", "type": 1, "application_id": "10", "version": "5", "name": "wolfram", "description": "Ask",
         "options": [{"type": 3, "name": "question", "required": true}, {"type": 4, "name": "num_solutions"}]},
        {"id": "3", "type": 1, "application_id": "20", "version": "6", "name": "quote", "options": [{"type": 3, "name": "ticker", "required": true}]},
        {"id": "4", "type": 1, "application_id": "10", "version": "7", "name": "quote", "options": [{"type": 3, "name": "stock", "required": true}]},
        {"id": "5", "type": 1, "application_id": "10", "version": "8", "name": "admin",
         "options": [{"type": 1, "name": "kick", "options": [{"type": 6, "name": "who", "required": true}, {"type": 5, "name": "loud"}]},
                     {"type": 2, "name": "role", "options": [{"type": 1, "name": "give", "options": [{"type": 8, "name": "role", "required": true}]}]}]},
        {"id": "6", "type": 4, "application_id": "20", "name": "launch"},
        {"id": "7", "type": 1, "application_id": "20", "version": "9", "name": "remind", "options": [{"type": 3, "name": "reminder", "required": true, "autocomplete": true}]}
      ]})");
    CommandIndex ix;
    ReadCommandIndex(index, ix);
    CHECK_EQ(ix.commands.size(), size_t(7));  // admin kick and admin role give; no launch
    CHECK_EQ(ix.apps[10].name, std::string("Rosamund"));

    size_t rest = 0;
    const Command* c = MatchCommand(ix.commands, "/weather", 0, &rest);
    CHECK(c && c->path == "weather");
    CHECK(!MatchCommand(ix.commands, "/weath", 0, &rest));
    CHECK(!MatchCommand(ix.commands, "/weathers x", 0, &rest));
    c = MatchCommand(ix.commands, "/admin role give role: <@&9>", 0, &rest);
    CHECK(c && c->path == "admin role give");
    CHECK_EQ(rest, size_t(16));
    // Two apps' /quote: the one picked wins.
    c = MatchCommand(ix.commands, "/quote NVDA", 4, &rest);
    CHECK(c && c->id == Snowflake(4));

    std::vector<std::pair<std::string, std::string>> values;
    std::string err;
    json out;
    // A bare value goes to the main option.
    const Command* w = MatchCommand(ix.commands, "/wolfram", 0, &rest);
    CHECK(SplitOptions(*w, " what is 2+2 num_solutions: 3", &values, &err));
    CHECK(OptionsJson(*w, values, {}, "", &out, &err));
    CHECK_EQ(out.dump(), json::parse(R"([{"type":3,"name":"question","value":"what is 2+2"},{"type":4,"name":"num_solutions","value":3}])").dump());
    CHECK(SplitOptions(*w, " NUM_SOLUTIONS: x question: hi", &values, &err));
    CHECK(!OptionsJson(*w, values, {}, "", &out, &err));
    CHECK_EQ(err, std::string("num_solutions needs a whole number."));
    CHECK(SplitOptions(*w, " num_solutions: 2", &values, &err));
    CHECK(!OptionsJson(*w, values, {}, "", &out, &err));
    CHECK(err.find("needs question") != std::string::npos);

    // Choices by name or value.
    const Command* wx = MatchCommand(ix.commands, "/weather", 0, &rest);
    CHECK(SplitOptions(*wx, " location: Bristol, UK units: imperial", &values, &err));
    CHECK(OptionsJson(*wx, values, {}, "", &out, &err));
    CHECK_EQ(out[0]["value"], json("Bristol, UK"));
    CHECK_EQ(out[1]["value"], json("fps"));
    CHECK(SplitOptions(*wx, " units: kelvin", &values, &err));
    CHECK(!OptionsJson(*wx, values, {}, "", &out, &err));
    CHECK_EQ(err, std::string("units has to be one of: Metric, Imperial."));

    // Subcommands nest; mentions read as IDs.
    const Command* kick = MatchCommand(ix.commands, "/admin kick", 0, &rest);
    CHECK(SplitOptions(*kick, " who: <@!42> loud: yes", &values, &err));
    CHECK(OptionsJson(*kick, values, {}, "", &out, &err));
    CHECK_EQ(out.dump(), json::parse(R"([{"type":1,"name":"kick","options":[{"type":6,"name":"who","value":"42"},{"type":5,"name":"loud","value":true}]}])").dump());

    // Where the cursor is.
    std::string name;
    size_t at = 0;
    std::string typed = " location: Bris units: met";
    CHECK(!OptionAt(*wx, typed, 0, &name, &at));
    CHECK(OptionAt(*wx, typed, 15, &name, &at));
    CHECK_EQ(name, std::string("location"));
    CHECK_EQ(at, size_t(11));
    CHECK(OptionAt(*wx, typed, typed.size(), &name, &at));
    CHECK_EQ(name, std::string("units"));

    // Autocomplete: the focused value goes as typed, and a picked
    // suggestion sends its value.
    const Command* rm = MatchCommand(ix.commands, "/remind", 0, &rest);
    CHECK(SplitOptions(*rm, " reminder: bin", &values, &err));
    CHECK(OptionsJson(*rm, values, {}, "reminder", &out, &err));
    CHECK_EQ(out[0]["focused"], json(true));
    CHECK(SplitOptions(*rm, " reminder: Bins out", &values, &err));
    CHECK(OptionsJson(*rm, values, {{"reminder", {{"Bins out", json(17)}}}}, "", &out, &err));
    CHECK_EQ(out[0]["value"], json(17));
}

int main() {
    testTime();
    testBase64AndProto();
    testReady();
    testVoice();
    testRtp();
    testH264();
    testPermissions();
    testMessages();
    testMentions();
    testMemberList();
    testMarkdown();
    testEmoji();
    testGif();
    testCommands();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("All tests passed\n");
    return 0;
}
