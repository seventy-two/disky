#include "store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace disky {

Snowflake ParseId(const json& j) {
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        return std::strtoull(s.c_str(), nullptr, 10);
    }
    if (j.is_number_unsigned()) return j.get<uint64_t>();
    if (j.is_number_integer()) return uint64_t(j.get<int64_t>());
    return 0;
}

std::string IdString(Snowflake id) { return std::to_string(id); }

int64_t IdTime(Snowflake id) { return int64_t(id >> 22) + kDiscordEpoch; }

namespace {

// daysFromCivil counts days from 1970-01-01 to a date (Howard Hinnant's).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int num(const json& j, const char* key, int def = 0) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : def;
}

bool flag(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

Snowflake id(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() ? ParseId(*it) : 0;
}

uint64_t bits(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_string()) return std::strtoull(it->get_ref<const std::string&>().c_str(), nullptr, 10);
    if (it->is_number()) return it->get<uint64_t>();
    return 0;
}

const json& arr(const json& j, const char* key) {
    static const json empty = json::array();
    auto it = j.find(key);
    return it != j.end() && it->is_array() ? *it : empty;
}

// entries reads Discord's versioned lists, {entries: [...]}, or a bare
// array as unversioned clients get.
const json& entries(const json& j) {
    static const json empty = json::array();
    if (j.is_array()) return j;
    if (j.is_object()) return arr(j, "entries");
    return empty;
}

Media readMedia(const json& j) {
    Media m;
    if (!j.is_object()) return m;
    m.url = str(j, "url");
    m.proxyUrl = str(j, "proxy_url");
    m.width = num(j, "width");
    m.height = num(j, "height");
    return m;
}

Emoji readEmoji(const json& j) {
    Emoji e;
    if (!j.is_object()) return e;
    e.id = id(j, "id");
    e.name = str(j, "name");
    e.animated = flag(j, "animated");
    return e;
}

Role readRole(const json& r) {
    Role role;
    role.id = id(r, "id");
    role.name = str(r, "name");
    role.colour = uint32_t(num(r, "color"));
    // Gradient roles carry their colours here; the first is the main one.
    if (auto c = r.find("colors"); c != r.end() && c->is_object() && (*c)["primary_color"].is_number())
        role.colour = (*c)["primary_color"].get<uint32_t>();
    role.position = num(r, "position");
    role.permissions = bits(r, "permissions");
    role.hoist = flag(r, "hoist");
    return role;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string lower(std::string s) {
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}

bool endsWithAny(const std::string& name, std::initializer_list<const char*> exts) {
    std::string n = lower(name);
    for (const char* e : exts) {
        size_t len = strlen(e);
        if (n.size() >= len && n.compare(n.size() - len, len, e) == 0) return true;
    }
    return false;
}

}  // namespace

int64_t ParseTime(const std::string& s) {
    int y, mo, d, h, mi, sec;
    if (s.size() < 19 || sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    int64_t ms = 0;
    size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        int digits = 0;
        for (++i; i < s.size() && isdigit((unsigned char)s[i]); ++i)
            if (digits < 3) {
                ms = ms * 10 + (s[i] - '0');
                digits++;
            }
        while (digits++ < 3) ms *= 10;
    }
    int64_t offset = 0;  // minutes east of UTC
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        int oh = 0, om = 0;
        sscanf(s.c_str() + i + 1, "%d:%d", &oh, &om);
        offset = (oh * 60 + om) * (s[i] == '-' ? -1 : 1);
    }
    int64_t days = daysFromCivil(y, unsigned(mo), unsigned(d));
    return ((days * 24 + h) * 60 + mi - offset) * 60000 + int64_t(sec) * 1000 + ms;
}

bool Attachment::IsImage() const {
    if (startsWith(contentType, "image/")) return true;
    return contentType.empty() && endsWithAny(filename, {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"});
}

bool Attachment::IsVideo() const {
    if (startsWith(contentType, "video/")) return true;
    return contentType.empty() && endsWithAny(filename, {".mp4", ".mov", ".webm", ".m4v"});
}

// --- base64 and the settings protobuf ---

std::string Base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int buf = 0, n = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        n += 6;
        if (n >= 8) {
            n -= 8;
            out.push_back(char((buf >> n) & 0xFF));
        }
    }
    return out;
}

std::string Base64Encode(const std::string& in) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += abc[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

namespace {

// Proto walks a protobuf message's fields.
struct Proto {
    const uint8_t* p;
    const uint8_t* end;
    bool varint(uint64_t& v) {
        v = 0;
        for (int shift = 0; p < end && shift < 64; shift += 7) {
            uint8_t b = *p++;
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    // next reads a field's key and value: varints and fixed64 into v,
    // length-delimited into sub. It returns false at the end or on bad data.
    bool next(uint32_t& field, int& wire, uint64_t& v, Proto& sub) {
        uint64_t key;
        if (p >= end || !varint(key)) return false;
        field = uint32_t(key >> 3);
        wire = int(key & 7);
        switch (wire) {
        case 0: return varint(v);
        case 1:
            if (end - p < 8) return false;
            memcpy(&v, p, 8);
            p += 8;
            return true;
        case 2: {
            uint64_t len;
            if (!varint(len) || uint64_t(end - p) < len) return false;
            sub = {p, p + len};
            p += len;
            return true;
        }
        case 5:
            if (end - p < 4) return false;
            p += 4;
            return true;
        default: return false;
        }
    }
};

// fixed64s reads a repeated fixed64 field, packed or not.
void fixed64s(int wire, uint64_t v, Proto sub, std::vector<Snowflake>& out) {
    if (wire == 1) {
        out.push_back(v);
        return;
    }
    if (wire != 2) return;
    while (sub.end - sub.p >= 8) {
        uint64_t x;
        memcpy(&x, sub.p, 8);
        sub.p += 8;
        out.push_back(x);
    }
}

}  // namespace

std::vector<Snowflake> ReadProtoGuildOrder(const std::string& bytes) {
    std::vector<Snowflake> order;
    Proto top{(const uint8_t*)bytes.data(), (const uint8_t*)bytes.data() + bytes.size()};
    uint32_t f;
    int w;
    uint64_t v;
    Proto sub{};
    while (top.next(f, w, v, sub)) {
        if (f != 14 || w != 2) continue;  // guild_folders
        std::vector<Snowflake> positions, inFolders;
        Proto folders = sub, inner{};
        while (folders.next(f, w, v, inner)) {
            if (f == 1 && w == 2) {  // a folder
                Proto folder = inner, ids{};
                while (folder.next(f, w, v, ids))
                    if (f == 1) fixed64s(w, v, ids, inFolders);
            } else if (f == 2) {  // guild_positions
                fixed64s(w, v, inner, positions);
            }
        }
        order = inFolders.empty() ? positions : inFolders;
    }
    return order;
}

// --- reading objects ---

void Store::readUser(const json& j) {
    if (!j.is_object()) return;
    Snowflake uid = id(j, "id");
    if (!uid) return;
    User& u = users[uid];
    u.id = uid;
    if (j.contains("username")) u.username = str(j, "username");
    if (j.contains("global_name")) u.globalName = str(j, "global_name");
    if (j.contains("avatar")) u.avatar = str(j, "avatar");
    if (j.contains("bot")) u.bot = flag(j, "bot");
}

Channel Store::readChannel(const json& j, Snowflake guildId) {
    Channel c;
    c.id = id(j, "id");
    c.guildId = guildId ? guildId : id(j, "guild_id");
    c.parentId = id(j, "parent_id");
    c.type = num(j, "type");
    c.name = str(j, "name");
    c.topic = str(j, "topic");
    c.icon = str(j, "icon");
    c.position = num(j, "position");
    c.lastMessageId = id(j, "last_message_id");
    for (const auto& o : arr(j, "permission_overwrites"))
        c.overwrites.push_back({id(o, "id"), num(o, "type"), bits(o, "allow"), bits(o, "deny")});
    for (const auto& r : arr(j, "recipient_ids")) c.recipients.push_back(ParseId(r));
    for (const auto& r : arr(j, "recipients")) {
        readUser(r);
        c.recipients.push_back(id(r, "id"));
    }
    return c;
}

void Store::readMember(Snowflake guildId, const json& j) {
    if (!j.is_object()) return;
    Snowflake uid = id(j, "user_id");
    if (auto u = j.find("user"); u != j.end() && u->is_object()) {
        readUser(*u);
        uid = id(*u, "id");
    }
    auto gi = guilds.find(guildId);
    if (!uid || gi == guilds.end()) return;
    Member& m = gi->second.members[uid];
    m.userId = uid;
    if (j.contains("nick")) m.nick = str(j, "nick");
    if (j.contains("avatar")) m.avatar = str(j, "avatar");
    if (j.contains("roles")) {
        m.roles.clear();
        for (const auto& r : arr(j, "roles")) m.roles.push_back(ParseId(r));
    }
}

void Store::readGuild(const json& j) {
    Snowflake gid = id(j, "id");
    if (!gid) return;
    Guild& g = guilds[gid];
    g.id = gid;
    if (flag(j, "unavailable")) {
        g.unavailable = true;
        return;
    }
    g.unavailable = false;
    // With the CLIENT_STATE_V2 capability, the guild's own fields are under
    // "properties"; without it, at the top level.
    const json& props = j.contains("properties") && j["properties"].is_object() ? j["properties"] : j;
    g.name = str(props, "name");
    g.icon = str(props, "icon");
    g.ownerId = id(props, "owner_id");
    if (j.contains("roles")) {
        g.roles.clear();
        for (const auto& r : arr(j, "roles")) {
            Role role = readRole(r);
            g.roles[role.id] = role;
        }
    }
    if (j.contains("channels")) {
        for (Snowflake c : g.channels) channels.erase(c);
        g.channels.clear();
        for (const auto& c : arr(j, "channels")) {
            Channel ch = readChannel(c, gid);
            g.channels.push_back(ch.id);
            channels[ch.id] = std::move(ch);
        }
    }
    if (j.contains("emojis")) {
        g.emojis.clear();
        for (const auto& e : arr(j, "emojis")) g.emojis.push_back(readEmoji(e));
    }
    for (const auto& m : arr(j, "members")) readMember(gid, m);
}

void Store::readGuildSettings(const json& e) {
    Snowflake gid = id(e, "guild_id");  // 0 for DMs
    GuildSettings& s = guildSettings[gid];
    s.muted = flag(e, "muted");
    // A timed mute that's ended no longer counts. The mute's end isn't
    // checked against the clock here: Discord sends an update when it ends.
    s.suppressEveryone = flag(e, "suppress_everyone");
    s.suppressRoles = flag(e, "suppress_roles");
    s.level = num(e, "message_notifications", 3);
    s.mutedChannels.clear();
    for (const auto& o : arr(e, "channel_overrides"))
        if (flag(o, "muted")) s.mutedChannels.insert(id(o, "channel_id"));
    // Settings outlive membership: only guilds we're in take them.
    if (auto g = guilds.find(gid); g != guilds.end()) g->second.muted = s.muted;
}

void Store::readSettingsProto(const std::string& base64) {
    std::vector<Snowflake> order = ReadProtoGuildOrder(Base64Decode(base64));
    if (!order.empty()) guildOrder = std::move(order);
}

Message Store::ParseMessage(const json& j) {
    Message m;
    m.id = id(j, "id");
    m.channelId = id(j, "channel_id");
    m.guildId = id(j, "guild_id");
    if (!m.guildId)
        if (auto c = channels.find(m.channelId); c != channels.end()) m.guildId = c->second.guildId;
    m.type = num(j, "type");
    m.content = str(j, "content");
    m.timestamp = ParseTime(str(j, "timestamp"));
    m.edited = ParseTime(str(j, "edited_timestamp"));
    m.nonce = j.contains("nonce") && j["nonce"].is_string() ? j["nonce"].get<std::string>()
              : j.contains("nonce") && j["nonce"].is_number() ? std::to_string(j["nonce"].get<uint64_t>())
                                                               : std::string();
    if (auto a = j.find("author"); a != j.end() && a->is_object()) {
        readUser(*a);
        m.authorId = id(*a, "id");
    }
    if (m.guildId) {
        if (auto mem = j.find("member"); mem != j.end() && mem->is_object()) {
            json copy = *mem;
            copy["user_id"] = IdString(m.authorId);
            readMember(m.guildId, copy);
        }
    }
    for (const auto& u : arr(j, "mentions")) {
        readUser(u);
        m.mentions.push_back(id(u, "id"));
        if (m.guildId)
            if (auto mem = u.find("member"); mem != u.end() && mem->is_object()) {
                json copy = *mem;
                copy["user_id"] = IdString(id(u, "id"));
                readMember(m.guildId, copy);
            }
    }
    for (const auto& r : arr(j, "mention_roles")) m.mentionRoles.push_back(ParseId(r));
    m.mentionEveryone = flag(j, "mention_everyone");
    for (const auto& a : arr(j, "attachments")) {
        Attachment at;
        at.id = id(a, "id");
        at.filename = str(a, "filename");
        at.url = str(a, "url");
        at.proxyUrl = str(a, "proxy_url");
        at.contentType = str(a, "content_type");
        at.width = num(a, "width");
        at.height = num(a, "height");
        at.size = a.contains("size") && a["size"].is_number() ? a["size"].get<uint64_t>() : 0;
        at.spoiler = startsWith(at.filename, "SPOILER_") || (num(a, "flags") & 8);
        m.attachments.push_back(std::move(at));
    }
    for (const auto& e : arr(j, "embeds")) {
        Embed em;
        em.type = str(e, "type");
        em.title = str(e, "title");
        em.description = str(e, "description");
        em.url = str(e, "url");
        if (e.contains("color") && e["color"].is_number()) {
            em.colour = e["color"].get<uint32_t>();
            em.hasColour = true;
        }
        if (auto p = e.find("provider"); p != e.end() && p->is_object()) em.provider = str(*p, "name");
        if (auto p = e.find("author"); p != e.end() && p->is_object()) em.author = str(*p, "name");
        if (auto p = e.find("footer"); p != e.end() && p->is_object()) em.footer = str(*p, "text");
        if (auto p = e.find("thumbnail"); p != e.end()) em.thumbnail = readMedia(*p);
        if (auto p = e.find("image"); p != e.end()) em.image = readMedia(*p);
        if (auto p = e.find("video"); p != e.end()) em.video = readMedia(*p);
        for (const auto& f : arr(e, "fields")) em.fields.push_back({str(f, "name"), str(f, "value"), flag(f, "inline")});
        m.embeds.push_back(std::move(em));
    }
    for (const auto& r : arr(j, "reactions")) {
        Reaction re;
        re.emoji = readEmoji(r["emoji"]);
        re.count = num(r, "count");
        re.me = flag(r, "me");
        m.reactions.push_back(std::move(re));
    }
    for (const auto& s : arr(j, "sticker_items")) m.stickers.push_back({id(s, "id"), str(s, "name"), num(s, "format_type", 1)});
    if (auto ref = j.find("message_reference"); ref != j.end() && ref->is_object() && num(*ref, "type") == 0 &&
                                                (m.type == 19 || j.contains("referenced_message")))
        m.replyTo = id(*ref, "message_id");
    if (m.replyTo) {
        Message::Reply r;
        auto rm = j.find("referenced_message");
        if (rm != j.end() && rm->is_object()) {
            if (auto a = rm->find("author"); a != rm->end() && a->is_object()) {
                readUser(*a);
                r.authorId = id(*a, "id");
            }
            r.content = str(*rm, "content");
            r.attachments = !arr(*rm, "attachments").empty() || !arr(*rm, "embeds").empty();
        } else {
            r.deleted = true;
        }
        m.reply = r;
    }
    return m;
}

Message* Store::findMessage(Snowflake channelId, Snowflake mid) {
    auto it = messages.find(channelId);
    if (it == messages.end()) return nullptr;
    // Newest first, as most lookups are for recent messages. Pending
    // messages at the end are out of order, so this doesn't bisect.
    auto& list = it->second.list;
    for (auto m = list.rbegin(); m != list.rend(); ++m)
        if (m->id == mid && !m->pending) return &*m;
    return nullptr;
}

// --- events ---

std::vector<Effect> Store::Apply(const std::string& type, const json& d) {
    std::vector<Effect> fx;
    if (type == "READY") {
        users.clear();
        guilds.clear();
        channels.clear();
        messages.clear();  // whatever was loaded may have gaps now
        reads.clear();
        guildSettings.clear();
        asked_.clear();
        if (auto u = d.find("user"); u != d.end()) {
            readUser(*u);
            me = id(*u, "id");
        }
        for (const auto& u : arr(d, "users")) readUser(u);
        const json& gs = arr(d, "guilds");
        for (const auto& g : gs) readGuild(g);
        const json& merged = arr(d, "merged_members");
        for (size_t i = 0; i < merged.size() && i < gs.size(); i++)
            for (const auto& m : merged[i]) readMember(id(gs[i], "id"), m);
        for (const auto& c : arr(d, "private_channels")) {
            Channel ch = readChannel(c, 0);
            channels[ch.id] = std::move(ch);
        }
        if (auto rs = d.find("read_state"); rs != d.end())
            for (const auto& r : entries(*rs)) {
                if (num(r, "read_state_type") != 0) continue;
                reads[id(r, "id")] = {id(r, "last_message_id"), num(r, "mention_count")};
            }
        if (auto s = d.find("user_guild_settings"); s != d.end())
            for (const auto& e : entries(*s)) readGuildSettings(e);
        if (auto p = d.find("user_settings_proto"); p != d.end() && p->is_string()) readSettingsProto(p->get<std::string>());
        fx.push_back({Effect::Type::Ready});
    } else if (type == "READY_SUPPLEMENTAL") {
        const json& gs = arr(d, "guilds");
        const json& merged = arr(d, "merged_members");
        for (size_t i = 0; i < merged.size() && i < gs.size(); i++)
            for (const auto& m : merged[i]) readMember(id(gs[i], "id"), m);
        if (auto p = d.find("merged_presences"); p != d.end() && p->is_object()) {
            for (const auto& f : arr(*p, "friends")) presence[id(f, "user_id")] = str(f, "status");
            for (const auto& g : arr(*p, "guilds"))
                for (const auto& pr : g) presence[id(pr, "user_id")] = str(pr, "status");
        }
    } else if (type == "GUILD_CREATE" || type == "GUILD_UPDATE") {
        readGuild(d);
    } else if (type == "GUILD_DELETE") {
        Snowflake gid = id(d, "id");
        if (flag(d, "unavailable")) {
            if (auto g = guilds.find(gid); g != guilds.end()) g->second.unavailable = true;
        } else {
            if (auto g = guilds.find(gid); g != guilds.end())
                for (Snowflake c : g->second.channels) channels.erase(c);
            guilds.erase(gid);
            fx.push_back({Effect::Type::GuildGone, 0, gid});
        }
    } else if (type == "GUILD_ROLE_CREATE" || type == "GUILD_ROLE_UPDATE") {
        Snowflake gid = id(d, "guild_id");
        if (auto r = d.find("role"); r != d.end() && guilds.count(gid)) {
            Role role = readRole(*r);
            guilds[gid].roles[role.id] = role;
        }
    } else if (type == "GUILD_ROLE_DELETE") {
        if (auto g = guilds.find(id(d, "guild_id")); g != guilds.end()) g->second.roles.erase(id(d, "role_id"));
    } else if (type == "GUILD_EMOJIS_UPDATE") {
        if (auto g = guilds.find(id(d, "guild_id")); g != guilds.end()) {
            g->second.emojis.clear();
            for (const auto& e : arr(d, "emojis")) g->second.emojis.push_back(readEmoji(e));
        }
    } else if (type == "GUILD_MEMBER_UPDATE" || type == "GUILD_MEMBER_ADD") {
        readMember(id(d, "guild_id"), d);
    } else if (type == "GUILD_MEMBERS_CHUNK") {
        Snowflake gid = id(d, "guild_id");
        for (const auto& m : arr(d, "members")) readMember(gid, m);
        for (const auto& p : arr(d, "presences"))
            if (auto u = p.find("user"); u != p.end()) presence[id(*u, "id")] = str(p, "status");
    } else if (type == "GUILD_MEMBER_LIST_UPDATE") {
        memberListUpdate(d);
    } else if (type == "PRESENCE_UPDATE") {
        if (auto u = d.find("user"); u != d.end()) {
            Snowflake uid = id(*u, "id");
            presence[uid] = str(d, "status");
            if (u->contains("username")) readUser(*u);
        }
    } else if (type == "CHANNEL_CREATE" || type == "CHANNEL_UPDATE") {
        Channel ch = readChannel(d, 0);
        if (!ch.id) return fx;
        if (ch.guildId) {
            auto g = guilds.find(ch.guildId);
            if (g == guilds.end()) return fx;
            auto& list = g->second.channels;
            if (std::find(list.begin(), list.end(), ch.id) == list.end()) list.push_back(ch.id);
        }
        // An update may leave out recipients and the last message.
        if (auto old = channels.find(ch.id); old != channels.end()) {
            if (ch.recipients.empty()) ch.recipients = old->second.recipients;
            if (!d.contains("last_message_id")) ch.lastMessageId = old->second.lastMessageId;
        }
        channels[ch.id] = std::move(ch);
    } else if (type == "CHANNEL_DELETE") {
        Snowflake cid = id(d, "id");
        if (auto c = channels.find(cid); c != channels.end()) {
            if (auto g = guilds.find(c->second.guildId); g != guilds.end()) {
                auto& list = g->second.channels;
                list.erase(std::remove(list.begin(), list.end(), cid), list.end());
            }
            channels.erase(c);
        }
        messages.erase(cid);
        fx.push_back({Effect::Type::ChannelGone, cid});
    } else if (type == "MESSAGE_CREATE") {
        Message m = ParseMessage(d);
        auto ch = channels.find(m.channelId);
        if (ch != channels.end() && m.id > ch->second.lastMessageId) ch->second.lastMessageId = m.id;
        if (auto it = messages.find(m.channelId); it != messages.end() && it->second.loaded) {
            auto& list = it->second.list;
            // Our own message replaces its pending copy.
            auto pend = std::find_if(list.begin(), list.end(),
                                     [&](const Message& x) { return x.pending && !m.nonce.empty() && x.nonce == m.nonce; });
            if (pend != list.end()) list.erase(pend);
            if (!findMessage(m.channelId, m.id)) {
                // In order by ID, with pending messages kept at the end.
                auto pos = std::find_if(list.begin(), list.end(), [&](const Message& x) { return x.pending || x.id > m.id; });
                list.insert(pos, m);
            }
        }
        if (m.authorId == me) {
            reads[m.channelId].lastAcked = std::max(reads[m.channelId].lastAcked, m.id);
            reads[m.channelId].mentions = 0;
        } else if (MentionsMe(m)) {
            reads[m.channelId].mentions++;
            fx.push_back({Effect::Type::Mention, m.channelId, m.guildId, m.id});
        }
    } else if (type == "MESSAGE_UPDATE") {
        Snowflake cid = id(d, "channel_id");
        if (Message* old = findMessage(cid, id(d, "id"))) {
            Message m = ParseMessage(d);
            if (d.contains("content")) old->content = m.content;
            if (d.contains("edited_timestamp")) old->edited = m.edited;
            if (d.contains("embeds")) old->embeds = m.embeds;
            if (d.contains("attachments")) old->attachments = m.attachments;
            if (d.contains("mentions")) old->mentions = m.mentions;
            if (d.contains("mention_roles")) old->mentionRoles = m.mentionRoles;
            if (d.contains("mention_everyone")) old->mentionEveryone = m.mentionEveryone;
            if (d.contains("reactions")) old->reactions = m.reactions;
        }
    } else if (type == "MESSAGE_DELETE") {
        RemoveMessage(id(d, "channel_id"), id(d, "id"));
    } else if (type == "MESSAGE_DELETE_BULK") {
        for (const auto& mid : arr(d, "ids")) RemoveMessage(id(d, "channel_id"), ParseId(mid));
    } else if (type == "MESSAGE_REACTION_ADD") {
        reactionAdd(d, true);
    } else if (type == "MESSAGE_REACTION_REMOVE") {
        reactionAdd(d, false);
    } else if (type == "MESSAGE_REACTION_ADD_MANY") {
        Snowflake cid = id(d, "channel_id"), mid = id(d, "message_id");
        for (const auto& r : arr(d, "reactions"))
            for (const auto& u : arr(r, "users"))
                reactionAdd({{"channel_id", IdString(cid)}, {"message_id", IdString(mid)}, {"user_id", u}, {"emoji", r["emoji"]}},
                            true);
    } else if (type == "MESSAGE_REACTION_REMOVE_ALL") {
        if (Message* m = findMessage(id(d, "channel_id"), id(d, "message_id"))) m->reactions.clear();
    } else if (type == "MESSAGE_REACTION_REMOVE_EMOJI") {
        if (Message* m = findMessage(id(d, "channel_id"), id(d, "message_id"))) {
            Emoji e = readEmoji(d["emoji"]);
            m->reactions.erase(std::remove_if(m->reactions.begin(), m->reactions.end(),
                                              [&](const Reaction& r) { return r.emoji == e; }),
                               m->reactions.end());
        }
    } else if (type == "MESSAGE_ACK") {
        ReadState& r = reads[id(d, "channel_id")];
        r.lastAcked = std::max(r.lastAcked, id(d, "message_id"));
        r.mentions = num(d, "mention_count");
    } else if (type == "USER_UPDATE") {
        readUser(d);
    } else if (type == "USER_GUILD_SETTINGS_UPDATE") {
        readGuildSettings(d);
    } else if (type == "USER_SETTINGS_PROTO_UPDATE") {
        if (auto s = d.find("settings"); s != d.end() && s->is_object() && num(*s, "type") == 1)
            readSettingsProto(str(*s, "proto"));
    }
    return fx;
}

void Store::reactionAdd(const json& d, bool add) {
    Message* m = findMessage(id(d, "channel_id"), id(d, "message_id"));
    if (!m) return;
    Emoji e = readEmoji(d["emoji"]);
    bool mine = id(d, "user_id") == me;
    auto it = std::find_if(m->reactions.begin(), m->reactions.end(), [&](const Reaction& r) { return r.emoji == e; });
    if (add) {
        if (it == m->reactions.end()) {
            m->reactions.push_back({e, 0, false});
            it = m->reactions.end() - 1;
        }
        if (mine && it->me) return;  // already shown when we sent it
        it->count++;
        if (mine) it->me = true;
    } else {
        if (it == m->reactions.end()) return;
        if (mine && !it->me) return;
        if (mine) it->me = false;
        if (--it->count <= 0) m->reactions.erase(it);
    }
}

void Store::memberListUpdate(const json& d) {
    Snowflake gid = id(d, "guild_id");
    auto g = guilds.find(gid);
    if (g == guilds.end()) return;
    MemberList& list = g->second.lists[str(d, "id")];
    list.id = str(d, "id");
    list.memberCount = num(d, "member_count", list.memberCount);
    list.onlineCount = num(d, "online_count", list.onlineCount);
    if (d.contains("groups"))
        for (const auto& g : arr(d, "groups")) list.groupCounts[str(g, "id")] = num(g, "count");
    auto item = [&](const json& j) {
        MemberList::Item it;
        if (auto grp = j.find("group"); grp != j.end()) {
            it.group = str(*grp, "id");
            it.count = num(*grp, "count");
        } else if (auto mem = j.find("member"); mem != j.end()) {
            readMember(gid, *mem);
            if (auto u = mem->find("user"); u != mem->end()) it.userId = id(*u, "id");
            if (auto p = mem->find("presence"); p != mem->end() && p->is_object()) {
                it.status = str(*p, "status");
                presence[it.userId] = it.status;
            }
            if (it.status.empty()) it.status = "offline";
        }
        return it;
    };
    for (const auto& op : arr(d, "ops")) {
        std::string kind = str(op, "op");
        if (kind == "SYNC" || kind == "INVALIDATE") {
            const json& range = arr(op, "range");
            if (range.size() != 2) continue;
            size_t from = range[0].get<size_t>(), to = range[1].get<size_t>();
            if (list.items.size() < to + 1 && kind == "SYNC") list.items.resize(to + 1);
            for (size_t i = from; i <= to && i < list.items.size(); i++) list.items[i] = {};
            if (kind == "SYNC") {
                size_t i = from;
                for (const auto& it : arr(op, "items")) {
                    if (i >= list.items.size()) list.items.resize(i + 1);
                    list.items[i++] = item(it);
                }
            }
        } else if (kind == "INSERT") {
            size_t i = size_t(num(op, "index"));
            if (i > list.items.size()) list.items.resize(i);
            list.items.insert(list.items.begin() + i, item(op["item"]));
        } else if (kind == "UPDATE") {
            size_t i = size_t(num(op, "index"));
            if (i >= list.items.size()) list.items.resize(i + 1);
            list.items[i] = item(op["item"]);
        } else if (kind == "DELETE") {
            size_t i = size_t(num(op, "index"));
            if (i < list.items.size()) list.items.erase(list.items.begin() + i);
        }
    }
}

void Store::Page(Snowflake channelId, const json& page, Snowflake before, int limit) {
    Messages& ms = messages[channelId];
    if (before == 0) ms.loaded = true;
    ms.loading = false;
    ms.loadingOlder = false;
    if (!page.is_array()) return;
    if (int(page.size()) < limit) ms.reachedStart = true;
    std::vector<Message> add;
    for (const auto& j : page) add.push_back(ParseMessage(j));
    std::vector<Message> pending;
    std::deque<Message> merged;
    for (auto& m : ms.list) (m.pending ? pending : add).push_back(std::move(m));
    std::sort(add.begin(), add.end(), [](const Message& a, const Message& b) { return a.id < b.id; });
    for (auto& m : add)
        if (merged.empty() || merged.back().id != m.id) merged.push_back(std::move(m));
    for (auto& m : pending) merged.push_back(std::move(m));
    ms.list = std::move(merged);
}

void Store::AddPending(const Message& m) {
    Messages& ms = messages[m.channelId];
    ms.list.push_back(m);
    ms.list.back().pending = true;
}

void Store::FailPending(Snowflake channelId, const std::string& nonce) {
    for (auto& m : messages[channelId].list)
        if (m.pending && m.nonce == nonce) m.failed = true;
}

void Store::RemoveMessage(Snowflake channelId, Snowflake mid) {
    auto it = messages.find(channelId);
    if (it == messages.end()) return;
    auto& list = it->second.list;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Message& m) { return !m.pending && m.id == mid; }),
               list.end());
    for (auto& m : list)
        if (m.replyTo == mid && m.reply) m.reply->deleted = true;
}

void Store::Acked(Snowflake channelId, Snowflake messageId) {
    ReadState& r = reads[channelId];
    r.lastAcked = std::max(r.lastAcked, messageId);
    r.mentions = 0;
}

void Store::NoteAsked(Snowflake guildId, const std::vector<Snowflake>& ids) {
    for (Snowflake u : ids) asked_.insert({guildId, u});
}

// --- queries ---

const User* Store::FindUser(Snowflake uid) const {
    auto it = users.find(uid);
    return it == users.end() ? nullptr : &it->second;
}

const Member* Store::FindMember(Snowflake guildId, Snowflake userId) const {
    auto g = guilds.find(guildId);
    if (g == guilds.end()) return nullptr;
    auto m = g->second.members.find(userId);
    return m == g->second.members.end() ? nullptr : &m->second;
}

std::string Store::DisplayName(Snowflake guildId, Snowflake userId) const {
    if (const Member* m = FindMember(guildId, userId); m && !m->nick.empty()) return m->nick;
    if (const User* u = FindUser(userId)) return u->Name();
    return "Unknown";
}

uint32_t Store::Colour(Snowflake guildId, Snowflake userId) const {
    const Member* m = FindMember(guildId, userId);
    if (!m) return 0;
    const Guild& g = guilds.at(guildId);
    const Role* best = nullptr;
    for (Snowflake r : m->roles) {
        auto it = g.roles.find(r);
        if (it == g.roles.end() || it->second.colour == 0) continue;
        if (!best || it->second.position > best->position) best = &it->second;
    }
    return best ? best->colour : 0;
}

uint64_t Store::Permissions(Snowflake channelId, Snowflake userId) const {
    auto c = channels.find(channelId);
    if (c == channels.end()) return 0;
    const Channel& ch = c->second;
    if (!ch.guildId) return perm::All;
    auto g = guilds.find(ch.guildId);
    if (g == guilds.end()) return 0;
    const Guild& guild = g->second;
    if (guild.ownerId == userId) return perm::All;
    const Member* m = FindMember(ch.guildId, userId);
    uint64_t p = 0;
    if (auto e = guild.roles.find(guild.id); e != guild.roles.end()) p = e->second.permissions;  // @everyone
    if (m)
        for (Snowflake r : m->roles)
            if (auto it = guild.roles.find(r); it != guild.roles.end()) p |= it->second.permissions;
    if (p & perm::Administrator) return perm::All;

    uint64_t allow = 0, deny = 0;
    for (const auto& o : ch.overwrites)
        if (o.type == 0 && o.id == guild.id) {
            p &= ~o.deny;
            p |= o.allow;
        }
    if (m)
        for (const auto& o : ch.overwrites)
            if (o.type == 0 && std::find(m->roles.begin(), m->roles.end(), o.id) != m->roles.end()) {
                allow |= o.allow;
                deny |= o.deny;
            }
    p &= ~deny;
    p |= allow;
    for (const auto& o : ch.overwrites)
        if (o.type == 1 && o.id == userId) {
            p &= ~o.deny;
            p |= o.allow;
        }
    return p;
}

bool Store::CanView(Snowflake channelId) const { return (Permissions(channelId, me) & perm::ViewChannel) != 0; }

std::string Store::ChannelName(Snowflake channelId) const {
    auto c = channels.find(channelId);
    if (c == channels.end()) return "unknown";
    const Channel& ch = c->second;
    if (!ch.IsPrivate()) return ch.name;
    if (!ch.name.empty()) return ch.name;
    std::string names;
    for (Snowflake r : ch.recipients) {
        if (!names.empty()) names += ", ";
        const User* u = FindUser(r);
        names += u ? u->Name() : "Unknown";
    }
    return names.empty() ? "Unnamed" : names;
}

std::vector<Snowflake> Store::PrivateChannels() const {
    std::vector<const Channel*> list;
    for (const auto& [cid, c] : channels)
        if (c.IsPrivate()) list.push_back(&c);
    std::sort(list.begin(), list.end(), [](const Channel* a, const Channel* b) {
        Snowflake la = std::max(a->lastMessageId, a->id), lb = std::max(b->lastMessageId, b->id);
        return la > lb;
    });
    std::vector<Snowflake> out;
    for (const Channel* c : list) out.push_back(c->id);
    return out;
}

std::vector<Snowflake> Store::GuildsInOrder() const {
    std::vector<Snowflake> rest, out;
    std::set<Snowflake> placed;
    for (Snowflake g : guildOrder)
        if (guilds.count(g) && placed.insert(g).second) out.push_back(g);
    for (const auto& [gid, g] : guilds)
        if (!placed.count(gid)) rest.push_back(gid);
    std::sort(rest.begin(), rest.end(), std::greater<>());
    rest.insert(rest.end(), out.begin(), out.end());
    return rest;
}

std::vector<Snowflake> Store::GuildChannels(Snowflake guildId) const {
    auto g = guilds.find(guildId);
    if (g == guilds.end()) return {};
    std::vector<const Channel*> cats, texts;
    for (Snowflake cid : g->second.channels) {
        auto c = channels.find(cid);
        if (c == channels.end()) continue;
        if (c->second.type == chan::Category)
            cats.push_back(&c->second);
        else if (c->second.IsText() && CanView(cid))
            texts.push_back(&c->second);
    }
    auto order = [](const Channel* a, const Channel* b) {
        return a->position != b->position ? a->position < b->position : a->id < b->id;
    };
    std::sort(cats.begin(), cats.end(), order);
    std::sort(texts.begin(), texts.end(), order);
    std::vector<Snowflake> out;
    for (const Channel* t : texts)
        if (!t->parentId) out.push_back(t->id);
    for (const Channel* cat : cats) {
        bool any = false;
        for (const Channel* t : texts)
            if (t->parentId == cat->id) {
                if (!any) out.push_back(cat->id);
                any = true;
                out.push_back(t->id);
            }
    }
    return out;
}

bool Store::ChannelMuted(Snowflake channelId) const {
    auto c = channels.find(channelId);
    if (c == channels.end()) return false;
    auto s = guildSettings.find(c->second.guildId);
    if (s == guildSettings.end()) return false;
    return s->second.mutedChannels.count(channelId) || (c->second.parentId && s->second.mutedChannels.count(c->second.parentId));
}

bool Store::Unread(Snowflake channelId) const {
    auto c = channels.find(channelId);
    if (c == channels.end() || !c->second.lastMessageId) return false;
    auto r = reads.find(channelId);
    // With no read state, Discord counts a channel as read.
    if (r == reads.end()) return false;
    return c->second.lastMessageId > r->second.lastAcked;
}

int Store::Mentions(Snowflake channelId) const {
    auto r = reads.find(channelId);
    return r == reads.end() ? 0 : r->second.mentions;
}

bool Store::GuildUnread(Snowflake guildId) const {
    auto g = guilds.find(guildId);
    if (g == guilds.end() || g->second.muted) return false;
    for (Snowflake c : GuildChannels(guildId))
        if (Unread(c) && !ChannelMuted(c)) return true;
    return false;
}

int Store::GuildMentions(Snowflake guildId) const {
    int n = 0;
    for (Snowflake c : GuildChannels(guildId)) n += Mentions(c);
    return n;
}

bool Store::MentionsMe(const Message& m) const {
    if (m.authorId == me) return false;
    auto c = channels.find(m.channelId);
    if (c == channels.end()) return false;
    GuildSettings s;
    if (auto it = guildSettings.find(c->second.guildId); it != guildSettings.end()) s = it->second;
    if (s.mutedChannels.count(m.channelId)) return false;
    if (c->second.IsPrivate()) return !s.muted;
    if (s.level == 2) return false;  // nothing
    if (std::find(m.mentions.begin(), m.mentions.end(), me) != m.mentions.end()) return true;
    if (m.mentionEveryone && !s.suppressEveryone) return true;
    if (!s.suppressRoles && !m.mentionRoles.empty())
        if (const Member* mem = FindMember(c->second.guildId, me))
            for (Snowflake r : m.mentionRoles)
                if (std::find(mem->roles.begin(), mem->roles.end(), r) != mem->roles.end()) return true;
    return false;
}

std::vector<Snowflake> Store::MissingMembers(Snowflake channelId) const {
    std::vector<Snowflake> out;
    auto c = channels.find(channelId);
    auto ms = messages.find(channelId);
    if (c == channels.end() || !c->second.guildId || ms == messages.end()) return out;
    Snowflake gid = c->second.guildId;
    std::set<Snowflake> seen;
    auto want = [&](Snowflake u) {
        if (u && !FindMember(gid, u) && !asked_.count({gid, u}) && seen.insert(u).second) out.push_back(u);
    };
    for (const auto& m : ms->second.list) {
        // Webhook messages have no member.
        if (const User* u = FindUser(m.authorId); u && u->bot && m.type != 0 && m.type != 19) continue;
        want(m.authorId);
        if (m.reply) want(m.reply->authorId);
        for (Snowflake u : m.mentions) want(u);
    }
    return out;
}

}  // namespace disky
