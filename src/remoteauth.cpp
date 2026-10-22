#include "remoteauth.h"

#include <bcrypt.h>

#include <chrono>
#include <condition_variable>
#include <vector>

namespace disky {

namespace {

// Key is an RSA-2048 key pair, made fresh for each sign-in.
class Key {
public:
    Key() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptGenerateKeyPair(alg_, &key_, 2048, 0) != 0 || BCryptFinalizeKeyPair(key_, 0) != 0) key_ = nullptr;
    }
    ~Key() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool ok() const { return key_ != nullptr; }

    // PublicDer is the public key as DER SubjectPublicKeyInfo, as Discord
    // wants it.
    std::string PublicDer() const {
        ULONG size = 0;
        BCryptExportKey(key_, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0);
        std::vector<uint8_t> blob(size);
        if (BCryptExportKey(key_, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0) != 0) return {};
        auto* h = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
        const uint8_t* p = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
        std::string exponent((const char*)p, h->cbPublicExp);
        std::string modulus((const char*)p + h->cbPublicExp, h->cbModulus);
        std::string rsa = seq(integer(modulus) + integer(exponent));
        // AlgorithmIdentifier: rsaEncryption (1.2.840.113549.1.1.1), NULL.
        static const char oid[] = "\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01\x05\x00";
        std::string algId = seq(std::string(oid, sizeof(oid) - 1));
        return seq(algId + tlv(0x03, std::string(1, '\0') + rsa));
    }

    // Decrypt undoes RSA-OAEP with SHA-256.
    bool Decrypt(const std::string& in, std::string& out) const {
        BCRYPT_OAEP_PADDING_INFO pad{BCRYPT_SHA256_ALGORITHM, nullptr, 0};
        ULONG size = 0;
        if (BCryptDecrypt(key_, (PUCHAR)in.data(), ULONG(in.size()), &pad, nullptr, 0, nullptr, 0, &size,
                          BCRYPT_PAD_OAEP) != 0)
            return false;
        out.resize(size);
        if (BCryptDecrypt(key_, (PUCHAR)in.data(), ULONG(in.size()), &pad, nullptr, 0, (PUCHAR)out.data(), size, &size,
                          BCRYPT_PAD_OAEP) != 0)
            return false;
        out.resize(size);
        return true;
    }

private:
    static std::string len(size_t n) {
        if (n < 128) return std::string(1, char(n));
        std::string b;
        while (n) {
            b.insert(b.begin(), char(n & 0xFF));
            n >>= 8;
        }
        return std::string(1, char(0x80 | b.size())) + b;
    }
    static std::string tlv(uint8_t tag, const std::string& body) { return std::string(1, char(tag)) + len(body.size()) + body; }
    static std::string seq(const std::string& body) { return tlv(0x30, body); }
    static std::string integer(std::string v) {
        size_t z = 0;
        while (z + 1 < v.size() && v[z] == 0) z++;
        v = v.substr(z);
        if ((uint8_t)v[0] & 0x80) v.insert(v.begin(), '\0');
        return tlv(0x02, v);
    }

    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

std::string sha256(const std::string& in) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out(32, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptHash(alg, nullptr, 0, (PUCHAR)in.data(), ULONG(in.size()), (PUCHAR)out.data(), 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string base64url(const std::string& in) {
    std::string s = Base64Encode(in);
    for (auto& c : s) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

}  // namespace

RemoteAuth::RemoteAuth(Identity id, std::function<void()> wake) : id_(std::move(id)), wake_(std::move(wake)) {}

RemoteAuth::~RemoteAuth() { Stop(); }

void RemoteAuth::Start() {
    Stop();
    stop_ = false;
    set([](Status& s) { s = Status(); });
    thread_ = std::thread(&RemoteAuth::run, this);
}

void RemoteAuth::Stop() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(wsMu_);
        if (ws_) ws_->Abort();
    }
    if (thread_.joinable()) thread_.join();
}

RemoteAuth::Status RemoteAuth::Get() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void RemoteAuth::set(const std::function<void(Status&)>& f) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        f(status_);
    }
    if (wake_) wake_();
}

void RemoteAuth::run() {
    // A code lasts a couple of minutes; when the server ends it, start a
    // new one, until signed in or stopped.
    for (int failures = 0; !stop_ && failures < 5;) {
        std::string error;
        if (attempt(error)) return;
        if (stop_) return;
        if (!error.empty()) {
            failures++;
            set([&](Status& s) {
                s.stage = Status::Stage::Failed;
                s.error = error;
            });
            if (failures >= 5) return;
            Sleep(2000);
        }
        set([](Status& s) {
            s.stage = Status::Stage::Connecting;
            s.qr.clear();
        });
    }
}

// attempt runs one code from start to end. It returns true once signed in;
// otherwise error says what went wrong, or is empty if the code expired.
bool RemoteAuth::attempt(std::string& error) {
    Key key;
    if (!key.ok()) {
        error = "couldn't make a key";
        return false;
    }
    auto ws = std::make_shared<WebSocket>(id_.userAgent);
    if (!ws->Connect("wss://remote-auth-gateway.discord.gg/?v=2", {{"Origin", "https://discord.com"}}, error)) return false;
    {
        std::lock_guard<std::mutex> lock(wsMu_);
        if (stop_) return false;
        ws_ = ws;
    }

    std::mutex hbMu;
    std::condition_variable hbCv;
    bool ended = false;
    std::thread heartbeat;
    auto finish = [&] {
        {
            std::lock_guard<std::mutex> lock(hbMu);
            ended = true;
        }
        hbCv.notify_all();
        if (heartbeat.joinable()) heartbeat.join();
        ws->Abort();
        std::lock_guard<std::mutex> lock(wsMu_);
        ws_.reset();
    };

    std::string msg, ticket;
    USHORT close = 0;
    while (ws->Receive(msg, error, &close)) {
        json j = json::parse(msg, nullptr, false);
        if (!j.is_object()) continue;
        std::string op = j.value("op", "");
        if (op == "hello") {
            int interval = j.value("heartbeat_interval", 41250);
            heartbeat = std::thread([&, interval] {
                for (;;) {
                    std::unique_lock<std::mutex> lock(hbMu);
                    if (hbCv.wait_for(lock, std::chrono::milliseconds(interval), [&] { return ended; })) return;
                    lock.unlock();
                    ws->Send(R"({"op":"heartbeat"})");
                }
            });
            ws->Send(json{{"op", "init"}, {"encoded_public_key", Base64Encode(key.PublicDer())}}.dump());
        } else if (op == "nonce_proof") {
            std::string nonce;
            if (!key.Decrypt(Base64Decode(j.value("encrypted_nonce", "")), nonce)) {
                error = "couldn't read Discord's challenge";
                break;
            }
            ws->Send(json{{"op", "nonce_proof"}, {"proof", base64url(sha256(nonce))}}.dump());
        } else if (op == "pending_remote_init") {
            std::string fp = j.value("fingerprint", "");
            set([&](Status& s) {
                s.stage = Status::Stage::Waiting;
                s.qr = "https://discord.com/ra/" + fp;
            });
        } else if (op == "pending_ticket") {
            std::string payload;
            if (key.Decrypt(Base64Decode(j.value("encrypted_user_payload", "")), payload)) {
                // id:discriminator:avatar:username
                std::vector<std::string> parts;
                size_t pos = 0;
                for (int i = 0; i < 3; i++) {
                    size_t c = payload.find(':', pos);
                    if (c == std::string::npos) break;
                    parts.push_back(payload.substr(pos, c - pos));
                    pos = c + 1;
                }
                parts.push_back(payload.substr(pos));
                set([&](Status& s) {
                    s.stage = Status::Stage::Scanned;
                    if (parts.size() == 4) {
                        s.userId = parts[0];
                        s.avatar = parts[2];
                        s.username = parts[3];
                    }
                });
            }
        } else if (op == "pending_login") {
            ticket = j.value("ticket", "");
            break;
        } else if (op == "cancel") {
            error.clear();
            finish();
            return false;  // the phone said no: show a new code
        }
    }
    finish();
    if (ticket.empty()) {
        if (close == 1000 || close == 4003) error.clear();  // the code timed out
        return false;
    }

    Api api(id_, "");
    HttpResult r = api.Unauthed("POST", "/users/@me/remote-auth/login", {{"ticket", ticket}});
    json body = json::parse(r.body, nullptr, false);
    std::string token;
    if (r.ok() && body.is_object() && key.Decrypt(Base64Decode(body.value("encrypted_token", "")), token)) {
        set([&](Status& s) {
            s.stage = Status::Stage::Done;
            s.token = token;
        });
        return true;
    }
    if (body.is_object() && body.contains("captcha_key"))
        error = "Discord wants a captcha for this sign-in, which this app can't show. Paste a token instead.";
    else
        error = r.error.empty() ? "sign-in failed" : r.error;
    // Don't retry: a ticket can't be used twice.
    stop_ = true;
    set([&](Status& s) {
        s.stage = Status::Stage::Failed;
        s.error = error;
    });
    return false;
}

}  // namespace disky
