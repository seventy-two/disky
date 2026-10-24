// The client: state from the gateway and REST, drawn with ImGui.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "commands.h"
#include "config.h"
#include "discord.h"
#include "images.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "markdown.h"
#include "remoteauth.h"
#include "richtext.h"
#include "store.h"
#include "theme.h"
#include "video.h"
#include "voice.h"

namespace disky {

class App {
public:
    // wake is called from background threads when there's something new.
    App(HWND hwnd, ID3D11Device* device, std::function<void()> wake);
    ~App();

    void SetScale(float scale);
    void SetDark(bool dark);
    // Pump takes in what background threads have received, whether or not
    // the window's showing, so notifications still fire.
    void Pump();
    // BeforeFrame loads fonts text drawn last frame needed; it returns true
    // if it did, so another frame should be drawn. Call it before NewFrame.
    bool BeforeFrame();
    // Frame draws the UI, between ImGui::NewFrame and Render.
    void Frame();
    // Busy says frames are wanted continuously (a video's playing).
    bool Busy() const;
    bool InCaption(float x, float y) const;
    // DropFiles attaches files dropped on the window.
    void DropFiles(const std::vector<std::wstring>& paths);
    // SaveWindow remembers where the window is, for next time.
    void SaveWindow();

private:
    // A UI-thread callback, posted from a worker when its request's done.
    void post(std::function<void()> f);
    void signIn(const std::string& token);
    void signOut();
    void startGateway();
    void onDispatch(const Dispatch& d);

    // Actions.
    void open(uint64_t guild, uint64_t channel);
    void loadLatest(uint64_t channel);
    void loadOlder(uint64_t channel);
    void subscribe(uint64_t guild, uint64_t channel, int ranges);
    void askMembers(uint64_t channel);
    void ack(uint64_t channel);
    void send();
    void sendMessage(uint64_t channel, std::string content, uint64_t replyTo, std::vector<Api::File> files,
                     const std::string& nonce);
    void saveEdit();
    // Slash commands: the open channel's (null until they've loaded, which
    // loadCommands starts), the one the draft starts with, and running it.
    const CommandIndex* commandsHere();
    void loadCommands();
    const Command* typedCommand(size_t* rest);
    void runCommand(const Command& c, size_t rest);
    json interaction(int type, const Command& c, const json& options, const std::string& nonce) const;
    void requestAutocomplete(const Command& c, const std::string& options, const std::string& option);
    // Voice: joining a channel (or moving to another), leaving, and
    // connecting once Discord's said which voice server to use.
    void joinVoice(uint64_t guild, uint64_t channel);
    void leaveVoice();
    void sendVoiceState();
    void connectVoice();
    void dropVoice();
    void deleteMessage(uint64_t channel, uint64_t id);
    void react(uint64_t channel, uint64_t id, const Emoji& e, bool add);
    void attachFile(const std::wstring& path);
    bool pasteImage();
    // notifyMention adds a mention or DM to the notification panel.
    void notifyMention(const Message& m);
    // seeNotices marks a channel's notifications seen (0 for all).
    void seeNotices(uint64_t channel);

    // Drawing.
    void titleBar();
    void login();
    // serversButton is the title bar's server picker, at a screen
    // position; it returns its width. serverMenu is the menu it opens.
    float serversButton(ImVec2 at);
    void serverMenu();
    void guildIcon(ImDrawList* dl, uint64_t guild, ImVec2 at, float size);
    float mentionBadge(ImDrawList* dl, int count, float right, float y);
    // barToggle is an icon button in the title bar that shows or hides a
    // column; on draws it lit.
    bool barToggle(const char* id, const char* glyph, ImVec2 at, bool on, const char* tip, ImU32 tint = 0);
    void sidebar(float x, float y, float w, float h);
    void channelList(float w);
    void dmList(float w);
    // voiceList is the open server's voice channels and who's in them, in
    // a column of its own beside the channel list.
    void voiceList(float x, float y, float w, float h);
    // voicePanel is the bar under it while in a call: how it's going, and
    // deafen and leave buttons.
    void voicePanel(float x, float y, float w);
    void userPanel(float x, float y, float w);
    void chat(float x, float y, float w, float h);
    // channelHeader draws the open channel's name and topic in the title
    // bar, centred, clear of left and right pixels at either end.
    void channelHeader(float left, float right);
    void messages(float w, float h);
    float messageHeight(const Message& m, bool groupStart, float w);
    // message draws one message; alt puts it on the band colour.
    void message(const Message& m, bool groupStart, bool alt, float w, float h);
    float attachmentsHeight(const Message& m, float w);
    void attachments(const Message& m, float x, float y, float w);
    float embedHeight(const Embed& e, float w);
    void embed(const Message& m, const Embed& e, float x, float y, float w);
    // media draws an image or video. A GIF (gif set: a video in play, or
    // else still is a .gif) plays while hovered and opens link when clicked.
    void media(const std::string& key, const std::string& still, const std::string& play, const std::string& link,
               ImVec2 at, ImVec2 size, bool spoiler, bool gif);
    float reactionsHeight(const Message& m, float w);
    void reactions(const Message& m, float x, float y, float w);
    void toolbar(const Message& m, ImVec2 topRight);
    void composer(float x, float y, float w, float* height);
    void autocomplete(ImVec2 above, float w);
    void memberList(float x, float y, float w, float h);
    void emojiPicker();
    // bell is the title bar's notification button, glowing while there are
    // unseen ones; notices is the panel it opens.
    void bell(ImVec2 at, float w, float h);
    void notices();
    void settings();
    void avatar(uint64_t user, uint64_t guild, ImVec2 at, float size, bool status = false);

    // Helpers.
    const Layout& layoutFor(const Message& m, float w);
    // textLayout lays out other text (embeds, replies), cached by key.
    const Layout& textLayout(const std::string& key, const std::string& text, float w, uint64_t guild, float size,
                             bool muted);
    std::string avatarUrl(uint64_t user, uint64_t guild) const;
    std::string formatTime(int64_t ms, bool full) const;
    std::string timestampSpan(int64_t seconds, char format) const;
    ImU32 userColour(uint64_t guild, uint64_t user) const;
    ImTextureID texture(const std::string& url);
    // image is texture, but with play an animated GIF animates.
    ImTextureID image(const std::string& url, bool play);
    Resolver resolver(uint64_t guild);
    std::string channelTitle(uint64_t channel) const;
    bool canSend(uint64_t channel) const;
    uint64_t perms(uint64_t channel) const;
    // viewSignature is who a channel's overwrites let see it, for matching
    // channels that share a member list.
    std::vector<std::pair<uint64_t, uint64_t>> viewSignature(uint64_t channel) const;
    std::string encodeMentions(const std::string& text) const;

    HWND hwnd_;
    ID3D11Device* device_;
    std::function<void()> wake_;
    float scale_ = 1;
    bool dark_ = true;
    Fonts fonts_;
    Config config_;

    Identity identity_;
    std::unique_ptr<Api> api_;
    std::unique_ptr<Gateway> gateway_;
    std::unique_ptr<RemoteAuth> remote_;
    std::unique_ptr<Worker> sender_, fetcher_;  // sends keep their order; fetches don't wait on them
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<VideoPlayer> video_;
    // gif_ plays GIFs sent as video (Tenor, Giphy) while they're hovered;
    // gifWanted_ says one was this frame.
    std::unique_ptr<VideoPlayer> gif_;
    bool gifWanted_ = false;
    // listShownForDMs_: the channel list was hidden, and opened only to pick a
    // DM; it hides again on going back to a server.
    bool listShownForDMs_ = false;
    Store store_;
    bool ready_ = false;
    std::string status_;  // connection trouble, for the title bar

    std::mutex postMu_;
    std::vector<std::function<void()>> posted_;
    // Bumped on sign-out, so callbacks for the old account do nothing.
    int generation_ = 0;

    // Where we are.
    uint64_t guild_ = 0, channel_ = 0;
    std::map<uint64_t, uint64_t> lastChannel_;  // per guild
    std::unordered_map<uint64_t, std::string> listFor_;  // channel → member list ID
    std::unordered_map<uint64_t, int> listRanges_;       // channel → ranges subscribed
    uint64_t subscribedChannel_ = 0;
    std::unordered_map<uint64_t, unsigned long long> acked_;  // channel → when last acked
    // Notifications, newest first, kept while disky runs.
    struct Notice {
        uint64_t guild = 0, channel = 0, author = 0;
        std::string title, body;
        int64_t time = 0;  // Unix ms
        bool seen = false;
    };
    std::deque<Notice> notices_;
    bool noticesOpen_ = false;
    ImVec2 bellAt_;
    ImVec2 serversAt_;  // where the server menu opens  // the bell's bottom right, for placing the panel

    // Message list.
    struct Cached {
        float width = 0;
        size_t version = 0;
        Layout layout;
    };
    std::unordered_map<uint64_t, Cached> layouts_;
    std::unordered_map<std::string, Cached> textLayouts_;
    float firstShownY_ = 0;
    float composerH_ = 0;
    bool composerActive_ = false;
    ImRect composerRect_;  // the text box, last frame
    uint64_t contextMessage_ = 0;
    std::unordered_map<uint64_t, std::vector<int>> revealed_;  // spoilers shown, by message
    std::set<std::string> revealedMedia_;
    float scrollMax_ = 0, scrollY_ = 0;
    bool atBottom_ = true;
    uint64_t firstShown_ = 0;
    float totalHeight_ = 0;
    uint64_t hovered_ = 0;  // message under the mouse
    uint64_t confirmDelete_ = 0;

    // Who's typing where: channel → user → when it lapses (GetTickCount64).
    std::unordered_map<uint64_t, std::map<uint64_t, unsigned long long>> typing_;
    std::string typingLine(uint64_t channel);
    // Our own typing: what the box held last frame, and when we last told
    // Discord.
    size_t draftHash_ = 0;
    unsigned long long typingSent_ = 0;

    // Composer.
    std::vector<char> draft_ = std::vector<char>(8192, 0);
    std::unordered_map<uint64_t, std::string> drafts_;  // per channel, while elsewhere
    uint64_t replyTo_ = 0;
    uint64_t editing_ = 0;
    bool focusComposer_ = false;
    struct Pending {
        std::string name, bytes, type;
    };
    std::vector<Pending> files_;
    std::string composeError_;
    // Mentions picked from autocomplete, by the text inserted.
    std::map<std::string, std::string> mentionCodes_;
    struct Suggestion {
        std::string label, insert, code, image;
        ImU32 colour = 0;
        std::string detail;  // muted, after the label
        int start = -1;      // where the insert starts, if not the word's start
        Snowflake command = 0;
        std::string option;  // a value for this command option,
        json value;          // sent as this
    };
    std::vector<Suggestion> suggestions_;
    void suggest();
    // suggestCommands fills suggestions for a draft starting with "/"; false
    // leaves it to the usual ones (mentions in a user option).
    bool suggestCommands();
    bool passiveSuggestions_ = false;  // Tab picks, but Enter still sends
    void pick(const Suggestion& s);
    int suggestion_ = 0;
    int tokenStart_ = -1, tokenEnd_ = -1;  // the word being completed
    std::string tokenText_;
    int dismissedToken_ = -1;  // where Escape closed suggestions
    std::string pendingInsert_;
    int pendingStart_ = -1, pendingEnd_ = -1;
    static int composerCallback(ImGuiInputTextCallbackData* data);

    // Slash commands, by guild (or DM channel), with the user's own apps'.
    std::string sessionId_;  // the gateway's, which interactions name
    std::map<uint64_t, CommandIndex> commandIndex_;
    std::set<uint64_t> commandsLoading_;
    Snowflake commandPick_ = 0;  // the command picked, where two share a name
    std::map<std::string, std::map<std::string, json>> commandPicked_;  // option → shown → value
    std::map<std::string, std::string> commandNonces_;                  // sent, by nonce → "/path"
    std::string commandStatus_;
    double commandSentAt_ = 0;
    // Autocomplete: what's typed (and since when), what was last asked,
    // and the answer, for the option it was for.
    std::string acTyped_, acAsked_, acNonce_, acPendingFor_, acFor_;
    double acTypedAt_ = 0;
    std::vector<CommandChoice> acChoices_;

    // Voice: the channel we're in or joining (0 for none), what Discord's
    // told us to connect with, and the connection.
    uint64_t voiceGuild_ = 0, voiceChannel_ = 0;
    std::string voiceSession_, voiceToken_, voiceEndpoint_;
    std::unique_ptr<VoiceConnection> voice_;
    uint64_t volumeFor_ = 0;  // whose volume the popup's for

    // Emoji picker: for a reaction (message set) or the composer.
    bool pickerOpen_ = false;
    uint64_t pickerMessage_ = 0;
    ImVec2 pickerAt_;
    char pickerSearch_[64] = {};
    int pickerCategory_ = 0;

    // Login.
    char tokenBuf_[256] = {};
    std::string loginError_;

    bool settingsOpen_ = false;
    bool startChoice_ = false;

    // Title bar.
    float captionH_ = 0;
    std::vector<ImRect> noDrag_;
    double now_ = 0;  // ImGui time this frame
};

void OpenURL(const std::string& url);

}  // namespace disky
