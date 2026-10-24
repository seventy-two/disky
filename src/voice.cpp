#include "voice.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dave/dave.h"
#include "miniaudio/miniaudio.h"
#include "net.h"
#include "opus/opus.h"
#include "rtp.h"

namespace disky {

namespace {

using Clock = std::chrono::steady_clock;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Voice gateway opcodes.
namespace vop {
constexpr int Identify = 0, SelectProtocol = 1, Ready = 2, Heartbeat = 3, SessionDescription = 4, Speaking = 5,
              HeartbeatAck = 6, Resume = 7, Hello = 8, Resumed = 9, ClientsConnect = 11, ClientConnect = 12,
              ClientDisconnect = 13, DavePrepareTransition = 21, DaveExecuteTransition = 22, DaveReadyForTransition = 23,
              DavePrepareEpoch = 24, DaveExternalSender = 25, DaveKeyPackage = 26, DaveProposals = 27,
              DaveCommitWelcome = 28, DaveAnnounceCommit = 29, DaveWelcome = 30, DaveInvalidCommitWelcome = 31;
}

const uint8_t kSilence[] = {0xF8, 0xFF, 0xFE};

// log writes to %APPDATA%\disky\voice.log, started afresh for each
// connection. Never anything secret: no tokens or keys.
std::mutex logMu;
FILE* logFile = nullptr;
void openLog() {
    std::lock_guard<std::mutex> lock(logMu);
    if (logFile) fclose(logFile);
    wchar_t* appdata = nullptr;
    size_t n = 0;
    if (_wdupenv_s(&appdata, &n, L"APPDATA") || !appdata) return;
    std::wstring path = std::wstring(appdata) + L"\\disky\\voice.log";
    free(appdata);
    logFile = _wfopen(path.c_str(), L"w");
}
void vlog(const std::string& s) {
    std::lock_guard<std::mutex> lock(logMu);
    if (!logFile) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(logFile, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s.c_str());
    fflush(logFile);
}

// Aes is AES-256-GCM, from Windows.
class Aes {
public:
    ~Aes() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool SetKey(const std::vector<uint8_t>& secret) {
        if (!alg_) {
            if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
            if (BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0)
                return false;
        }
        if (key_) BCryptDestroyKey(key_);
        key_ = nullptr;
        return BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, (PUCHAR)secret.data(), ULONG(secret.size()), 0) >= 0;
    }
    bool Open(const uint8_t* nonce, const uint8_t* aad, size_t aadLen, const uint8_t* cipher, size_t len,
              const uint8_t* tag, std::string& out) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)nonce;
        info.cbNonce = 12;
        info.pbAuthData = (PUCHAR)aad;
        info.cbAuthData = ULONG(aadLen);
        info.pbTag = (PUCHAR)tag;
        info.cbTag = 16;
        out.resize(len);
        ULONG written = 0;
        if (BCryptDecrypt(key_, (PUCHAR)cipher, ULONG(len), &info, nullptr, 0, (PUCHAR)out.data(), ULONG(len), &written, 0) < 0)
            return false;
        out.resize(written);
        return true;
    }
    bool Seal(const uint8_t* nonce, const std::string& aad, const uint8_t* plain, size_t len, std::string& out) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        uint8_t tag[16];
        info.pbNonce = (PUCHAR)nonce;
        info.cbNonce = 12;
        info.pbAuthData = (PUCHAR)aad.data();
        info.cbAuthData = ULONG(aad.size());
        info.pbTag = tag;
        info.cbTag = 16;
        out.resize(len);
        ULONG written = 0;
        if (BCryptEncrypt(key_, (PUCHAR)plain, ULONG(len), &info, nullptr, 0, (PUCHAR)out.data(), ULONG(len), &written, 0) < 0)
            return false;
        out.resize(written);
        out.append(reinterpret_cast<char*>(tag), 16);
        return true;
    }
    bool Ready() const { return key_ != nullptr; }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// Dave keeps the MLS group and each speaker's decryptor, following
// libdave's own sample (DaveSessionManager). Its methods are called with
// the connection's dave mutex held; send sends to the voice gateway.
class Dave {
public:
    std::function<void(int op, const json& d)> sendJson;
    std::function<void(int op, const std::string& payload)> sendBinary;

    Dave(Snowflake self, Snowflake group) : self_(IdString(self)), group_(group) {
        session_ = daveSessionCreate(nullptr, "", [](const char* source, const char* reason, void*) {
            vlog(std::string("dave: MLS failure in ") + source + ": " + reason);
        }, nullptr);
    }
    ~Dave() {
        for (auto& [u, d] : decryptors_) daveDecryptorDestroy(d);
        if (session_) daveSessionDestroy(session_);
    }
    uint16_t Version() const { return version_; }

    void UserJoined(const std::string& user) {
        if (user == self_) return;
        recognised_.insert(user);
        setUp(user, latest_);
    }
    void UserLeft(const std::string& user) {
        recognised_.erase(user);
        if (auto it = decryptors_.find(user); it != decryptors_.end()) {
            daveDecryptorDestroy(it->second);
            decryptors_.erase(it);
        }
    }

    // Init is the session description's protocol version.
    void Init(uint16_t version) {
        vlog("dave: init v" + std::to_string(version));
        if (version > 0) {
            version_ = version;
            daveSessionInit(session_, version, group_, self_.c_str());
            sendKeyPackage();
        } else {
            prepare(0, version);
            execute(0);
        }
    }

    void Json(int op, const json& d) {
        if (op == vop::DavePrepareTransition) {
            int id = d.value("transition_id", 0), v = d.value("protocol_version", 0);
            vlog("dave: prepare transition " + std::to_string(id) + " to v" + std::to_string(v));
            prepare(id, uint16_t(v));
            if (id != 0) sendJson(vop::DaveReadyForTransition, {{"transition_id", id}});
        } else if (op == vop::DaveExecuteTransition) {
            int id = d.value("transition_id", 0);
            vlog("dave: execute transition " + std::to_string(id));
            execute(id);
        } else if (op == vop::DavePrepareEpoch) {
            int v = d.value("protocol_version", 0);
            // epoch can come as a number or a string.
            int64_t epoch = d.contains("epoch") ? (d["epoch"].is_string() ? std::stoll(d["epoch"].get<std::string>())
                                                                           : d["epoch"].get<int64_t>())
                                                : 0;
            vlog("dave: prepare epoch " + std::to_string(epoch) + " v" + std::to_string(v));
            if (epoch == 1) {
                version_ = uint16_t(v);
                daveSessionInit(session_, uint16_t(v), group_, self_.c_str());
                sendKeyPackage();
            }
        }
    }

    void Binary(int op, const std::string& p) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p.data());
        if (op == vop::DaveExternalSender) {
            vlog("dave: external sender");
            daveSessionSetExternalSender(session_, b, p.size());
        } else if (op == vop::DaveProposals) {
            auto ids = recognisedIds();
            std::vector<const char*> ptrs;
            for (const auto& s : ids) ptrs.push_back(s.c_str());
            uint8_t* out = nullptr;
            size_t n = 0;
            daveSessionProcessProposals(session_, b, p.size(), ptrs.data(), ptrs.size(), &out, &n);
            vlog("dave: proposals, " + std::to_string(n) + " bytes of commit back");
            if (out && n) sendBinary(vop::DaveCommitWelcome, std::string(reinterpret_cast<char*>(out), n));
            if (out) daveFree(out);
        } else if (op == vop::DaveAnnounceCommit && p.size() >= 2) {
            int id = b[0] << 8 | b[1];
            DAVECommitResultHandle r = daveSessionProcessCommit(session_, b + 2, p.size() - 2);
            bool ignored = r && daveCommitResultIsIgnored(r), failed = !r || daveCommitResultIsFailed(r);
            if (r) daveCommitResultDestroy(r);
            vlog("dave: commit for transition " + std::to_string(id) + (ignored ? " ignored" : failed ? " failed" : " joined"));
            if (ignored) return;
            if (failed) {
                sendJson(vop::DaveInvalidCommitWelcome, {{"transition_id", id}});
                Init(daveSessionGetProtocolVersion(session_));
                return;
            }
            prepare(id, daveSessionGetProtocolVersion(session_));
            if (id != 0) sendJson(vop::DaveReadyForTransition, {{"transition_id", id}});
        } else if (op == vop::DaveWelcome && p.size() >= 2) {
            int id = b[0] << 8 | b[1];
            auto ids = recognisedIds();
            std::vector<const char*> ptrs;
            for (const auto& s : ids) ptrs.push_back(s.c_str());
            DAVEWelcomeResultHandle r = daveSessionProcessWelcome(session_, b + 2, p.size() - 2, ptrs.data(), ptrs.size());
            vlog("dave: welcome for transition " + std::to_string(id) + (r ? " joined" : " failed"));
            if (r) {
                daveWelcomeResultDestroy(r);
                prepare(id, daveSessionGetProtocolVersion(session_));
                if (id != 0) sendJson(vop::DaveReadyForTransition, {{"transition_id", id}});
            } else {
                sendJson(vop::DaveInvalidCommitWelcome, {{"transition_id", id}});
                sendKeyPackage();
            }
        }
    }

    // Decrypt takes a speaker's frame out of end-to-end encryption; false
    // if it can't be yet (keys still being agreed).
    bool Decrypt(const std::string& user, const std::string& in, std::string& out) {
        auto it = decryptors_.find(user);
        if (it == decryptors_.end()) {
            // Unencrypted calls pass straight through.
            if (version_ == 0) {
                out = in;
                return true;
            }
            return false;
        }
        size_t cap = daveDecryptorGetMaxPlaintextByteSize(it->second, DAVE_MEDIA_TYPE_AUDIO, in.size());
        out.resize(cap);
        size_t n = 0;
        auto rc = daveDecryptorDecrypt(it->second, DAVE_MEDIA_TYPE_AUDIO, reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                       reinterpret_cast<uint8_t*>(out.data()), cap, &n);
        if (rc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) {
            if (++failures_ % 50 == 1) vlog("dave: decrypt failed (" + std::to_string(int(rc)) + "), " + std::to_string(failures_) + " so far");
            return false;
        }
        out.resize(n);
        return true;
    }

private:
    void sendKeyPackage() {
        uint8_t* kp = nullptr;
        size_t n = 0;
        daveSessionGetMarshalledKeyPackage(session_, &kp, &n);
        vlog("dave: sending key package, " + std::to_string(n) + " bytes");
        if (kp && n) sendBinary(vop::DaveKeyPackage, std::string(reinterpret_cast<char*>(kp), n));
        if (kp) daveFree(kp);
    }
    std::vector<std::string> recognisedIds() const {
        std::vector<std::string> ids(recognised_.begin(), recognised_.end());
        ids.push_back(self_);
        return ids;
    }
    // setUp gives a speaker the keys for a protocol version.
    void setUp(const std::string& user, uint16_t version) {
        if (user == self_) return;  // our own keys only matter for sending
        DAVEDecryptorHandle& d = decryptors_[user];
        if (!d) d = daveDecryptorCreate();
        if (version == 0) {
            daveDecryptorTransitionToPassthroughMode(d, true);
            return;
        }
        if (DAVEKeyRatchetHandle k = daveSessionGetKeyRatchet(session_, user.c_str())) {
            daveDecryptorTransitionToKeyRatchet(d, k);
            daveKeyRatchetDestroy(k);
        }
    }
    void prepare(int id, uint16_t version) {
        for (const auto& u : recognised_) setUp(u, version);
        if (id != 0) transitions_[id] = version;
        latest_ = version;
    }
    void execute(int id) {
        auto it = transitions_.find(id);
        if (it == transitions_.end()) return;
        uint16_t v = it->second;
        transitions_.erase(it);
        version_ = v;
        if (v == 0) daveSessionReset(session_);
    }

    std::string self_;
    uint64_t group_;
    DAVESessionHandle session_ = nullptr;
    std::set<std::string> recognised_;
    std::map<int, uint16_t> transitions_;
    std::unordered_map<std::string, DAVEDecryptorHandle> decryptors_;
    uint16_t latest_ = 0, version_ = 0;
    int failures_ = 0;
};

// Speaker is one person's audio coming in.
struct Speaker {
    Snowflake user = 0;
    OpusDecoder* decoder = nullptr;
    Playout playout;
    Clock::time_point heard{};  // last sound, not silence
    ~Speaker() {
        if (decoder) opus_decoder_destroy(decoder);
    }
};

}  // namespace

struct VoiceConnection::Impl {
    Params p;
    std::function<void()> wake;
    std::atomic<bool> stop{false};
    std::thread control, udpThread;

    mutable std::mutex mu;  // state, error, speakers, volumes
    State state = State::Connecting;
    std::string error;
    std::atomic<int> ping{0};
    std::atomic<bool> encrypted{false};
    std::unordered_map<uint32_t, std::unique_ptr<Speaker>> speakers;  // by SSRC
    std::unordered_map<Snowflake, uint32_t> ssrcOf;
    std::unordered_map<Snowflake, float> volumes;
    std::atomic<bool> deaf{false};
    std::atomic<float> outputGain{1.0f};

    std::mutex wsMu;
    std::shared_ptr<WebSocket> ws;
    std::atomic<int> seq{-1};

    std::mutex daveMu;
    std::unique_ptr<Dave> dave;

    // UDP and transport encryption.
    SOCKET sock = INVALID_SOCKET;
    uint32_t ssrc = 0;
    std::mutex aesMu;
    Aes aes;
    uint32_t sendNonce = 0;
    uint16_t sendSeq = 0;
    uint32_t sendTs = 0;

    ma_device device{};
    bool deviceUp = false;

    void setState(State s, const std::string& why = {}) {
        {
            std::lock_guard<std::mutex> lock(mu);
            state = s;
            if (!why.empty()) error = why;
        }
        wake();
    }

    bool sendJson(int op, const json& d) {
        std::shared_ptr<WebSocket> w;
        {
            std::lock_guard<std::mutex> lock(wsMu);
            w = ws;
        }
        return w && w->Send(json{{"op", op}, {"d", d}}.dump());
    }
    bool sendBinary(int op, const std::string& payload) {
        std::shared_ptr<WebSocket> w;
        {
            std::lock_guard<std::mutex> lock(wsMu);
            w = ws;
        }
        return w && w->SendBinary(VoiceBinary(op, payload));
    }

    void run();
    bool session(bool resume, std::string& err, USHORT& close);
    bool startUdp(const std::string& ip, int port, std::string& myIp, uint16_t& myPort, std::string& err);
    void receive();
    void packet(const uint8_t* b, size_t n);
    void sendSilence(int frames);
    void startAudio();
    static void audioCallback(ma_device* d, void* out, const void*, ma_uint32 frames);
};

void VoiceConnection::Impl::run() {
    openLog();
    vlog("connecting to the voice server for channel " + IdString(p.channelId));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    bool resume = false;
    int attempts = 0;
    while (!stop) {
        std::string err;
        USHORT close = 0;
        bool reached = session(resume, err, close);
        {
            std::lock_guard<std::mutex> lock(wsMu);
            ws.reset();
        }
        if (stop) break;
        vlog("voice gateway closed: " + err + " (" + std::to_string(close) + ")");
        // 4014: removed from the channel (or it's gone); 4006: the session's
        // no longer valid; 4004: refused. Starting again needs a new join.
        if (close == 4014 || close == 4006 || close == 4004 || close == 4011 || close == 4016) {
            setState(State::Failed, close == 4014 ? "Disconnected from the channel." : "The voice server ended the call (" + std::to_string(close) + ").");
            break;
        }
        if (reached) attempts = 0;
        if (++attempts > 5) {
            setState(State::Failed, "Lost the connection to the voice server: " + err);
            break;
        }
        resume = sock != INVALID_SOCKET;  // with UDP up, carry on where it was
        setState(State::Connecting);
        for (int i = 0; i < attempts * 10 && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool VoiceConnection::Impl::session(bool resume, std::string& err, USHORT& close) {
    auto w = std::make_shared<WebSocket>(p.userAgent);
    std::string url = "wss://" + p.endpoint + "/?v=8";
    if (!w->Connect(url, {{"Origin", "https://discord.com"}}, err)) return false;
    {
        std::lock_guard<std::mutex> lock(wsMu);
        if (stop) return false;
        ws = w;
    }

    std::mutex hbMu;
    std::condition_variable hbCv;
    bool ended = false;
    std::thread heartbeat;
    std::atomic<int64_t> lastSent{0}, lastAck{0};
    auto startHeartbeat = [&](int interval) {
        heartbeat = std::thread([&, interval] {
            int keepalive = 0;
            auto nextKeepalive = Clock::now();
            auto nextBeat = Clock::now();
            for (;;) {
                std::unique_lock<std::mutex> lock(hbMu);
                if (hbCv.wait_for(lock, std::chrono::milliseconds(250), [&] { return ended || stop.load(); })) return;
                lock.unlock();
                auto t = Clock::now();
                if (t >= nextBeat) {
                    // Two unanswered in a row and the connection's dead.
                    if (lastSent && lastAck < lastSent && nowMs() - lastSent > interval * 2) {
                        w->Abort();
                        return;
                    }
                    int64_t n = nowMs();
                    lastSent = n;
                    sendJson(vop::Heartbeat, {{"t", n}, {"seq_ack", seq.load()}});
                    nextBeat = t + std::chrono::milliseconds(interval);
                }
                // UDP keepalives, so the way back through routers stays open.
                if (t >= nextKeepalive && sock != INVALID_SOCKET) {
                    char k[8] = {};
                    memcpy(k, &keepalive, 4);
                    keepalive++;
                    ::send(sock, k, 8, 0);
                    nextKeepalive = t + std::chrono::seconds(5);
                }
            }
        });
    };
    auto finish = [&] {
        {
            std::lock_guard<std::mutex> lock(hbMu);
            ended = true;
        }
        hbCv.notify_all();
        if (heartbeat.joinable()) heartbeat.join();
    };

    std::string guild = IdString(p.guildId), user = IdString(p.userId);
    bool reached = false;
    std::string msg;
    bool binary = false;
    while (!stop) {
        if (!w->Receive(msg, err, &close, &binary)) break;
        if (binary) {
            int s = 0, op = 0;
            std::string payload;
            if (!ParseVoiceBinary(msg, &s, &op, &payload)) continue;
            seq = s;
            std::lock_guard<std::mutex> lock(daveMu);
            if (dave) dave->Binary(op, payload);
            continue;
        }
        json j = json::parse(msg, nullptr, false);
        if (!j.is_object()) continue;
        if (j.contains("seq") && j["seq"].is_number()) seq = j["seq"].get<int>();
        int op = j.value("op", -1);
        const json& d = j.contains("d") ? j["d"] : json();
        switch (op) {
        case vop::Hello: {
            int interval = int(d.value("heartbeat_interval", 13750.0));
            vlog("hello, heartbeat every " + std::to_string(interval) + "ms");
            startHeartbeat(interval);
            if (resume) {
                sendJson(vop::Resume, {{"server_id", guild}, {"session_id", p.sessionId}, {"token", p.token}, {"seq_ack", seq.load()}});
            } else {
                sendJson(vop::Identify, {{"server_id", guild},
                                         {"user_id", user},
                                         {"session_id", p.sessionId},
                                         {"token", p.token},
                                         {"max_dave_protocol_version", daveMaxSupportedProtocolVersion()}});
            }
            break;
        }
        case vop::Ready: {
            ssrc = d.value("ssrc", 0u);
            std::string ip = d.value("ip", "");
            int port = d.value("port", 0);
            bool gcm = false;
            for (const auto& m : d.value("modes", json::array()))
                if (m.is_string() && m.get<std::string>() == "aead_aes256_gcm_rtpsize") gcm = true;
            vlog("ready: ssrc " + std::to_string(ssrc) + ", " + ip + ":" + std::to_string(port) + (gcm ? "" : ", no AES-GCM"));
            if (!gcm) {
                err = "the voice server doesn't offer AES-GCM";
                setState(State::Failed, "The voice server wants encryption disky doesn't have.");
                stop = true;
                break;
            }
            std::string myIp;
            uint16_t myPort = 0;
            if (!startUdp(ip, port, myIp, myPort, err)) {
                vlog("UDP: " + err);
                setState(State::Failed, "Couldn't reach the voice server: " + err);
                stop = true;
                break;
            }
            vlog("UDP up");
            sendJson(vop::SelectProtocol,
                     {{"protocol", "udp"},
                      {"data", {{"address", myIp}, {"port", myPort}, {"mode", "aead_aes256_gcm_rtpsize"}}},
                      {"codecs", json::array({{{"name", "opus"}, {"type", "audio"}, {"priority", 1000}, {"payload_type", kOpusPayloadType}}})}});
            break;
        }
        case vop::SessionDescription: {
            std::vector<uint8_t> key;
            for (const auto& k : d.value("secret_key", json::array())) key.push_back(uint8_t(k.get<int>()));
            int daveVersion = d.value("dave_protocol_version", 0);
            vlog("session: mode " + d.value("mode", std::string()) + ", DAVE v" + std::to_string(daveVersion));
            {
                std::lock_guard<std::mutex> lock(aesMu);
                if (!aes.SetKey(key)) {
                    setState(State::Failed, "Couldn't set up the call's encryption.");
                    stop = true;
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(daveMu);
                dave->Init(uint16_t(daveVersion));
                encrypted = daveVersion > 0;
            }
            if (!udpThread.joinable()) udpThread = std::thread(&Impl::receive, this);
            startAudio();
            sendSilence(5);
            reached = true;
            setState(State::Connected);
            break;
        }
        case vop::Resumed:
            vlog("resumed");
            reached = true;
            setState(State::Connected);
            break;
        case vop::HeartbeatAck: {
            int64_t t = d.is_object() ? d.value("t", int64_t(0)) : d.is_number() ? d.get<int64_t>() : 0;
            lastAck = nowMs();
            if (t) ping = int(nowMs() - t);
            break;
        }
        case vop::Speaking:
        case vop::ClientConnect: {
            Snowflake uid = ParseId(d.value("user_id", json()));
            uint32_t s = op == vop::Speaking ? d.value("ssrc", 0u) : d.value("audio_ssrc", 0u);
            if (uid && s) {
                std::lock_guard<std::mutex> lock(mu);
                ssrcOf[uid] = s;
                auto& sp = speakers[s];
                if (!sp) sp = std::make_unique<Speaker>();
                sp->user = uid;
            }
            if (uid) {
                std::lock_guard<std::mutex> lock(daveMu);
                if (dave) dave->UserJoined(IdString(uid));
            }
            break;
        }
        case vop::ClientsConnect: {
            std::lock_guard<std::mutex> lock(daveMu);
            for (const auto& u : d.value("user_ids", json::array()))
                if (dave && u.is_string()) dave->UserJoined(u.get<std::string>());
            break;
        }
        case vop::ClientDisconnect: {
            Snowflake uid = ParseId(d.value("user_id", json()));
            {
                std::lock_guard<std::mutex> lock(daveMu);
                if (dave) dave->UserLeft(IdString(uid));
            }
            std::lock_guard<std::mutex> lock(mu);
            if (auto it = ssrcOf.find(uid); it != ssrcOf.end()) {
                speakers.erase(it->second);
                ssrcOf.erase(it);
            }
            break;
        }
        case vop::DavePrepareTransition:
        case vop::DaveExecuteTransition:
        case vop::DavePrepareEpoch: {
            std::lock_guard<std::mutex> lock(daveMu);
            if (dave) dave->Json(op, d);
            encrypted = dave && dave->Version() > 0;
            break;
        }
        default:
            break;
        }
    }
    w->Abort();
    finish();
    return reached;
}

bool VoiceConnection::Impl::startUdp(const std::string& ip, int port, std::string& myIp, uint16_t& myPort, std::string& err) {
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        err = "no socket";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1 || connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        err = "bad address " + ip;
        return false;
    }
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    std::string req = DiscoveryRequest(ssrc);
    uint8_t buf[2048];
    for (int tries = 0; tries < 5 && !stop; tries++) {
        ::send(sock, req.data(), int(req.size()), 0);
        int n = recv(sock, (char*)buf, sizeof(buf), 0);
        if (n > 0 && ParseDiscovery(buf, size_t(n), &myIp, &myPort)) return true;
    }
    err = "no answer to IP discovery";
    return false;
}

void VoiceConnection::Impl::receive() {
    uint8_t buf[4096];
    while (!stop) {
        int n = recv(sock, (char*)buf, sizeof(buf), 0);
        if (n <= 0) continue;  // the timeout, so stop's noticed
        packet(buf, size_t(n));
    }
}

void VoiceConnection::Impl::packet(const uint8_t* b, size_t n) {
    Rtp r;
    if (!ParseRtp(b, n, &r) || r.payloadType != kOpusPayloadType) return;
    Snowflake user = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = speakers.find(r.ssrc);
        if (it == speakers.end()) return;  // not yet known who it is
        user = it->second->user;
    }
    Sealed s;
    if (!SplitSealed(r, n, b, &s)) return;
    std::string plain;
    {
        std::lock_guard<std::mutex> lock(aesMu);
        if (!aes.Ready() || !aes.Open(s.nonce, b, r.header, b + s.cipher, s.cipherLen, b + s.tag, plain)) return;
    }
    if (!Unwrap(r, plain)) return;
    std::string frame;
    {
        std::lock_guard<std::mutex> lock(daveMu);
        if (!dave || !dave->Decrypt(IdString(user), plain, frame)) return;
    }
    bool silence = frame.size() == 3 && memcmp(frame.data(), kSilence, 3) == 0;
    bool startedSpeaking = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = speakers.find(r.ssrc);
        if (it == speakers.end()) return;
        Speaker& sp = *it->second;
        if (!sp.decoder) {
            int e = 0;
            sp.decoder = opus_decoder_create(kSampleRate, kChannels, &e);
            if (e != OPUS_OK) {
                sp.decoder = nullptr;
                return;
            }
        }
        int lost = sp.playout.Arrived(r.seq);
        if (lost < 0) return;
        float pcm[5760 * kChannels];
        for (int i = 0; i < lost; i++) {
            int got = opus_decode_float(sp.decoder, nullptr, 0, pcm, kFrameSamples, 0);
            if (got > 0) sp.playout.Push(pcm, size_t(got));
        }
        int got = opus_decode_float(sp.decoder, reinterpret_cast<const unsigned char*>(frame.data()), opus_int32(frame.size()), pcm,
                                    5760, 0);
        if (got > 0) sp.playout.Push(pcm, size_t(got));
        if (!silence) {
            auto t = Clock::now();
            startedSpeaking = t - sp.heard > std::chrono::milliseconds(300);
            sp.heard = t;
        }
    }
    if (startedSpeaking) wake();
}

void VoiceConnection::Impl::sendSilence(int frames) {
    for (int i = 0; i < frames; i++) {
        std::string header = RtpHeader(sendSeq++, sendTs, ssrc);
        sendTs += kFrameSamples;
        uint8_t nonce[12] = {};
        uint32_t c = sendNonce++;
        nonce[0] = uint8_t(c >> 24);
        nonce[1] = uint8_t(c >> 16);
        nonce[2] = uint8_t(c >> 8);
        nonce[3] = uint8_t(c);
        std::string sealed;
        {
            std::lock_guard<std::mutex> lock(aesMu);
            if (!aes.Seal(nonce, header, kSilence, sizeof(kSilence), sealed)) return;
        }
        std::string pkt = header + sealed + std::string(reinterpret_cast<char*>(nonce), 4);
        ::send(sock, pkt.data(), int(pkt.size()), 0);
    }
}

void VoiceConnection::Impl::startAudio() {
    if (deviceUp) return;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = audioCallback;
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
        vlog("audio: no output device");
        return;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        ma_device_uninit(&device);
        vlog("audio: output wouldn't start");
        return;
    }
    deviceUp = true;
}

void VoiceConnection::Impl::audioCallback(ma_device* d, void* out, const void*, ma_uint32 frames) {
    Impl* self = static_cast<Impl*>(d->pUserData);
    float* o = static_cast<float*>(out);
    memset(o, 0, size_t(frames) * kChannels * sizeof(float));
    std::lock_guard<std::mutex> lock(self->mu);
    bool deaf = self->deaf;
    float master = self->outputGain;
    for (auto& [ssrc, sp] : self->speakers) {
        float gain = master;
        if (auto v = self->volumes.find(sp->user); v != self->volumes.end()) gain *= v->second;
        if (deaf) gain = 0;
        sp->playout.Mix(o, frames, gain);  // drained even while deafened, so nothing piles up
    }
    for (size_t i = 0; i < size_t(frames) * kChannels; i++) o[i] = o[i] > 1 ? 1 : o[i] < -1 ? -1 : o[i];
}

VoiceConnection::VoiceConnection(Params p, std::function<void()> wake) : impl_(std::make_unique<Impl>()) {
    impl_->p = std::move(p);
    impl_->wake = std::move(wake);
    impl_->dave = std::make_unique<Dave>(impl_->p.userId, impl_->p.channelId);
    Impl* i = impl_.get();
    impl_->dave->sendJson = [i](int op, const json& d) { i->sendJson(op, d); };
    impl_->dave->sendBinary = [i](int op, const std::string& b) { i->sendBinary(op, b); };
    impl_->control = std::thread(&Impl::run, impl_.get());
}

VoiceConnection::~VoiceConnection() {
    impl_->stop = true;
    {
        std::lock_guard<std::mutex> lock(impl_->wsMu);
        if (impl_->ws) impl_->ws->Close();
    }
    if (impl_->control.joinable()) impl_->control.join();
    if (impl_->udpThread.joinable()) impl_->udpThread.join();
    if (impl_->deviceUp) ma_device_uninit(&impl_->device);
    if (impl_->sock != INVALID_SOCKET) closesocket(impl_->sock);
    vlog("closed");
}

VoiceConnection::State VoiceConnection::GetState() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state;
}

std::string VoiceConnection::Error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

int VoiceConnection::Ping() const { return impl_->ping; }
bool VoiceConnection::Encrypted() const { return impl_->encrypted; }

bool VoiceConnection::Speaking(Snowflake user) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto s = impl_->ssrcOf.find(user);
    if (s == impl_->ssrcOf.end()) return false;
    auto sp = impl_->speakers.find(s->second);
    return sp != impl_->speakers.end() && Clock::now() - sp->second->heard < std::chrono::milliseconds(300);
}

bool VoiceConnection::AnySpeaking() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto t = Clock::now();
    for (const auto& [ssrc, sp] : impl_->speakers)
        if (t - sp->heard < std::chrono::milliseconds(300)) return true;
    return false;
}

void VoiceConnection::SetVolume(Snowflake user, float gain) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->volumes[user] = gain;
}

void VoiceConnection::SetDeafened(bool deaf) { impl_->deaf = deaf; }
void VoiceConnection::SetOutputVolume(float gain) { impl_->outputGain = gain; }

}  // namespace disky
