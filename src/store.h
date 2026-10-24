// What the client knows about the account: guilds, channels, users,
// members, roles and the messages loaded so far, kept up to date from the
// gateway's events. Portable C++, so it's tested on Linux too.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace disky {

using json = nlohmann::json;
using Snowflake = uint64_t;

// Discord's epoch, 2015-01-01, in Unix milliseconds.
constexpr int64_t kDiscordEpoch = 1420070400000;

Snowflake ParseId(const json& j);
std::string IdString(Snowflake id);
// IdTime is when a snowflake was made, in Unix milliseconds.
int64_t IdTime(Snowflake id);
// ParseTime reads an ISO 8601 timestamp, as Discord sends them, into Unix
// milliseconds; 0 if it isn't one.
int64_t ParseTime(const std::string& s);

namespace perm {
constexpr uint64_t AddReactions = 1ull << 6;
constexpr uint64_t Administrator = 1ull << 3;
constexpr uint64_t ViewChannel = 1ull << 10;
constexpr uint64_t SendMessages = 1ull << 11;
constexpr uint64_t ManageMessages = 1ull << 13;
constexpr uint64_t AttachFiles = 1ull << 15;
constexpr uint64_t ReadHistory = 1ull << 16;
constexpr uint64_t MentionEveryone = 1ull << 17;
constexpr uint64_t All = ~0ull;
}  // namespace perm

namespace chan {
constexpr int Text = 0, DM = 1, Voice = 2, GroupDM = 3, Category = 4, News = 5, Stage = 13, Forum = 15;
constexpr int NewsThread = 10, PublicThread = 11, PrivateThread = 12;
}  // namespace chan

struct User {
    Snowflake id = 0;
    std::string username, globalName, avatar;
    bool bot = false;
    // Name is what to call them: their display name, else username.
    const std::string& Name() const { return globalName.empty() ? username : globalName; }
};

struct Role {
    Snowflake id = 0;
    std::string name;
    uint32_t colour = 0;  // 0 for none
    int position = 0;
    uint64_t permissions = 0;
    bool hoist = false;
};

struct Overwrite {
    Snowflake id = 0;
    int type = 0;  // 0 role, 1 member
    uint64_t allow = 0, deny = 0;
};

struct Channel {
    Snowflake id = 0, guildId = 0, parentId = 0;
    int type = 0;
    std::string name, topic, icon;
    int position = 0;
    std::vector<Overwrite> overwrites;
    Snowflake lastMessageId = 0;
    std::vector<Snowflake> recipients;  // DMs and group DMs, not including us
    bool IsText() const { return type == chan::Text || type == chan::News || type == chan::DM || type == chan::GroupDM; }
    bool IsVoice() const { return type == chan::Voice || type == chan::Stage; }
    bool IsPrivate() const { return type == chan::DM || type == chan::GroupDM; }
};

struct Member {
    Snowflake userId = 0;
    std::string nick, avatar;
    std::vector<Snowflake> roles;
};

// VoiceState is someone in a guild's voice channel.
struct VoiceState {
    Snowflake userId = 0, channelId = 0;
    bool selfMute = false, selfDeaf = false, mute = false, deaf = false;  // mute/deaf: by a moderator
    bool streaming = false, video = false;  // Go Live, camera
    bool suppress = false;                  // a stage's audience
};

struct Emoji {
    Snowflake id = 0;  // 0 for a Unicode emoji
    std::string name;  // the emoji itself for Unicode ones
    bool animated = false;
    bool operator==(const Emoji& o) const { return id ? id == o.id : name == o.name; }
};

// MemberList is a guild channel's member list as the gateway sends it:
// group headers (hoisted roles, online, offline) and members, in order.
struct MemberList {
    struct Item {
        std::string group;  // set for a header: a role ID, "online" or "offline"
        int count = 0;      // a header's member count
        Snowflake userId = 0;
        std::string status;  // a member's: online, idle, dnd, offline
    };
    std::string id;
    int memberCount = 0, onlineCount = 0;
    std::vector<Item> items;  // sparse: ranges not yet sent are empty items
    std::map<std::string, int> groupCounts;  // members in each group
};

struct Media {
    std::string url, proxyUrl;
    int width = 0, height = 0;
};

struct Attachment {
    Snowflake id = 0;
    std::string filename, url, proxyUrl, contentType;
    int width = 0, height = 0;
    uint64_t size = 0;
    bool spoiler = false;
    bool IsImage() const;
    bool IsVideo() const;
};

struct EmbedField {
    std::string name, value;
    bool inl = false;
};

struct Embed {
    std::string type, title, description, url, provider, author, footer;
    uint32_t colour = 0;
    bool hasColour = false;
    Media thumbnail, image, video;
    std::vector<EmbedField> fields;
};

struct Reaction {
    Emoji emoji;
    int count = 0;
    bool me = false;
};

struct Message {
    Snowflake id = 0, channelId = 0, guildId = 0, authorId = 0;
    int type = 0;
    int flags = 0;  // 64 only the reader sees it, 128 its app's still answering
    std::string content;
    int64_t timestamp = 0, edited = 0;  // Unix ms; edited is 0 if never
    std::vector<Attachment> attachments;
    std::vector<Embed> embeds;
    std::vector<Reaction> reactions;
    std::vector<Snowflake> mentions, mentionRoles;
    bool mentionEveryone = false;
    struct Sticker {
        Snowflake id = 0;
        std::string name;
        int format = 1;  // 1 PNG, 2 APNG, 3 Lottie, 4 GIF
    };
    std::vector<Sticker> stickers;
    // A reply's original, as it was sent with the reply.
    Snowflake replyTo = 0;
    struct Reply {
        Snowflake authorId = 0;
        std::string content;
        bool attachments = false;
        bool deleted = false;
    };
    std::optional<Reply> reply;
    // The command a bot's message answers: who ran it and its name, with
    // subcommands. Discord doesn't send the options they typed.
    struct Command {
        Snowflake userId = 0;
        std::string name;
    };
    std::optional<Command> command;
    // A message we've sent that the server hasn't confirmed: nonce is
    // what matches the two up.
    std::string nonce;
    bool pending = false, failed = false;
};

// Messages is what's loaded of one channel's history, oldest first.
struct Messages {
    std::deque<Message> list;
    bool loaded = false;     // the latest page has arrived
    bool reachedStart = false;  // nothing older to load
    bool loadingOlder = false;
    bool loading = false;
};

struct ReadState {
    Snowflake lastAcked = 0;
    int mentions = 0;
};

struct Guild {
    Snowflake id = 0, ownerId = 0;
    std::string name, icon;
    std::unordered_map<Snowflake, Role> roles;
    std::vector<Snowflake> channels;
    std::unordered_map<Snowflake, Member> members;
    std::vector<Emoji> emojis;
    std::map<std::string, MemberList> lists;  // by list ID
    std::unordered_map<Snowflake, VoiceState> voice;  // who's in voice, by user
    bool muted = false;
    bool unavailable = false;
};

// GuildSettings is how the user wants a guild's notifications: muted, and
// message_notifications 0 all, 1 mentions only, 2 nothing, 3 the guild default.
struct GuildSettings {
    bool muted = false;
    bool suppressEveryone = false, suppressRoles = false;
    int level = 3;
    std::set<Snowflake> mutedChannels;
};

// Effect is something an event did that the UI should hear about.
struct Effect {
    enum class Type { Ready, Mention, ChannelGone, GuildGone } type;
    Snowflake channelId = 0, guildId = 0, messageId = 0;
};

class Store {
public:
    // Apply takes one gateway dispatch, by event name, and returns what it
    // did that matters beyond the data, such as a message that mentions us.
    std::vector<Effect> Apply(const std::string& type, const json& d);

    // Page adds a page of history from the REST API, newest first as it
    // comes. before is the ID it was asked for older than, or 0 for the
    // latest page.
    void Page(Snowflake channelId, const json& page, Snowflake before, int limit);

    // AddPending shows a message we're sending before the server confirms it.
    void AddPending(const Message& m);
    void FailPending(Snowflake channelId, const std::string& nonce);
    void RemoveMessage(Snowflake channelId, Snowflake id);

    // Acked records that we've read a channel up to a message.
    void Acked(Snowflake channelId, Snowflake messageId);

    Snowflake me = 0;
    std::unordered_map<Snowflake, User> users;
    std::unordered_map<Snowflake, Guild> guilds;
    std::unordered_map<Snowflake, Channel> channels;
    std::unordered_map<Snowflake, Messages> messages;
    std::unordered_map<Snowflake, ReadState> reads;
    std::unordered_map<Snowflake, GuildSettings> guildSettings;
    std::unordered_map<Snowflake, std::string> presence;  // user → status
    // Guild order from the user's settings, folders flattened; guilds
    // missing from it go first, newest first, as Discord does.
    std::vector<Snowflake> guildOrder;
    // Private channels, most recent first.
    std::vector<Snowflake> PrivateChannels() const;
    std::vector<Snowflake> GuildsInOrder() const;

    const User* FindUser(Snowflake id) const;
    const Member* FindMember(Snowflake guildId, Snowflake userId) const;
    // DisplayName is a user's name in a guild: nickname, else display name,
    // else username; "Unknown" if we don't know them yet.
    std::string DisplayName(Snowflake guildId, Snowflake userId) const;
    // Colour is a member's name colour: their highest coloured role's, 0
    // for none (draw in the normal text colour).
    uint32_t Colour(Snowflake guildId, Snowflake userId) const;
    // Permissions works out what a user can do in a channel, from their
    // roles and the channel's overwrites. Private channels allow everything.
    uint64_t Permissions(Snowflake channelId, Snowflake userId) const;
    bool CanView(Snowflake channelId) const;
    // ChannelName is how to label a channel: a guild channel's name, or a
    // DM's other person, or a group DM's name or members.
    std::string ChannelName(Snowflake channelId) const;

    // Channels lists a guild's channels to show, in Discord's order: those
    // outside a category first, then each category with its channels. The
    // entries are categories (only those with something visible) and text
    // channels the user can see.
    std::vector<Snowflake> GuildChannels(Snowflake guildId) const;

    // VoiceChannels is the same for voice and stage channels.
    std::vector<Snowflake> VoiceChannels(Snowflake guildId) const;
    // InVoice lists who's in a voice channel, by name.
    std::vector<VoiceState> InVoice(Snowflake channelId) const;
    // VoiceCount is how many are in a guild's voice channels.
    int VoiceCount(Snowflake guildId) const;

    bool Unread(Snowflake channelId) const;
    int Mentions(Snowflake channelId) const;
    bool GuildUnread(Snowflake guildId) const;
    int GuildMentions(Snowflake guildId) const;
    bool ChannelMuted(Snowflake channelId) const;

    // MentionsMe reports whether a message should notify: it mentions us,
    // a role we have or everyone (as the guild's settings allow), or it's
    // a DM.
    bool MentionsMe(const Message& m) const;

    // MissingMembers lists authors in a channel's loaded messages whose
    // guild member we don't have, so their colours can be asked for.
    std::vector<Snowflake> MissingMembers(Snowflake channelId) const;

    // ParseMessage reads a message object.
    Message ParseMessage(const json& j);

private:
    void readUser(const json& j);
    void readGuild(const json& j);
    Channel readChannel(const json& j, Snowflake guildId);
    void readMember(Snowflake guildId, const json& j);
    void readVoiceState(Snowflake guildId, const json& j);
    void readGuildSettings(const json& j);
    void readSettingsProto(const std::string& base64);
    void memberListUpdate(const json& d);
    void reactionAdd(const json& d, bool add);
    Message* findMessage(Snowflake channelId, Snowflake id);
    // asked is who we've already asked the gateway about, by guild.
    std::set<std::pair<Snowflake, Snowflake>> asked_;

public:
    // NoteAsked marks members as asked for, so MissingMembers skips them.
    void NoteAsked(Snowflake guildId, const std::vector<Snowflake>& ids);
};

// ReadProtoGuildOrder pulls the guild order (folders flattened, in order)
// out of the user's settings protobuf, PreloadedUserSettings.guild_folders.
std::vector<Snowflake> ReadProtoGuildOrder(const std::string& bytes);
std::string Base64Decode(const std::string& in);
std::string Base64Encode(const std::string& in);

}  // namespace disky
