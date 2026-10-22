#include "discord.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <regex>

namespace disky {

namespace {

unsigned long long now() { return GetTickCount64(); }

std::string uuid() {
    std::random_device rd;
    std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd());
    uint64_t a = gen(), b = gen();
    a = (a & ~0xF000ull) | 0x4000ull;  // version 4
    b = (b & ~(0xC000ull << 48)) | (0x8000ull << 48);
    char buf[40];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", unsigned(a >> 32), unsigned((a >> 16) & 0xFFFF),
             unsigned(a & 0xFFFF), unsigned(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFull));
    return buf;
}

// edgeMajor is the installed Edge's major version, from where Edge
// records it; a recent one if it isn't installed.
int edgeMajor() {
    wchar_t buf[64];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Edge\\BLBeacon", L"version", RRF_RT_REG_SZ, nullptr, buf,
                     &size) == ERROR_SUCCESS) {
        int v = _wtoi(buf);
        if (v > 100) return v;
    }
    return 155;
}

// route groups a request for rate limiting: its method and path, with
// IDs other than a channel's or guild's replaced, as Discord buckets them.
std::string route(const std::string& method, const std::string& path) {
    std::string out = method + " ";
    std::string prev;
    size_t pos = 0;
    std::string p = path.substr(0, path.find('?'));
    while (pos < p.size()) {
        size_t end = p.find('/', pos + 1);
        if (end == std::string::npos) end = p.size();
        std::string seg = p.substr(pos, end - pos);
        bool numeric = seg.size() > 1 && std::all_of(seg.begin() + 1, seg.end(), ::isdigit);
        if (prev == "/reactions")
            out += "/:emoji";
        else if (numeric && prev != "/channels" && prev != "/guilds")
            out += "/:id";
        else
            out += seg;
        prev = seg;
        pos = end;
    }
    return out;
}

}  // namespace

std::string NewNonce() {
    using namespace std::chrono;
    int64_t ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return std::to_string(uint64_t(ms - kDiscordEpoch) << 22);
}

Identity MakeIdentity(int buildNumber) {
    Identity id;
    int major = edgeMajor();
    id.browserVersion = std::to_string(major) + ".0.0.0";
    id.userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" +
                   id.browserVersion + " Safari/537.36 Edg/" + id.browserVersion;
    id.buildNumber = buildNumber;
    id.properties = {
        {"os", "Windows"},
        {"browser", "Edge"},
        {"device", ""},
        {"system_locale", id.locale},
        {"has_client_mods", false},
        {"browser_user_agent", id.userAgent},
        {"browser_version", id.browserVersion},
        {"os_version", "10"},
        {"referrer", ""},
        {"referring_domain", ""},
        {"referrer_current", ""},
        {"referring_domain_current", ""},
        {"release_channel", "stable"},
        {"client_build_number", buildNumber},
        {"client_event_source", nullptr},
        {"client_launch_id", uuid()},
        {"client_heartbeat_session_id", uuid()},
    };
    id.superProps = Base64Encode(id.properties.dump());
    return id;
}

int FetchBuildNumber(Http& http) {
    HttpRequest req;
    req.url = "https://discord.com/app";
    req.maxBytes = 4 << 20;
    HttpResult r = http.Send(req);
    if (!r.ok()) return 0;
    std::smatch m;
    static const std::regex re("BUILD_NUMBER\"\\s*:\\s*\"(\\d+)\"");
    if (std::regex_search(r.body, m, re)) return std::stoi(m[1].str());
    return 0;
}

// --- REST ---

Api::Api(Identity id, std::string token) : id_(std::move(id)), token_(std::move(token)), http_(id_.userAgent) {}

Headers Api::headers(bool auth) const {
    Headers h = {
        {"Accept", "*/*"},
        {"Accept-Language", "en-GB,en;q=0.9"},
        {"Origin", "https://discord.com"},
        {"Referer", "https://discord.com/channels/@me"},
        {"X-Super-Properties", id_.superProps},
        {"X-Discord-Locale", id_.locale},
        {"X-Discord-Timezone", id_.timezone},
        {"X-Debug-Options", "bugReporterEnabled"},
    };
    if (auth) h.insert(h.begin(), {"Authorization", token_});
    return h;
}

HttpResult Api::send(HttpRequest req, const std::string& key) {
    HttpResult r;
    for (int attempt = 0; attempt < 5; attempt++) {
        // Wait for the global limit and this route's bucket.
        for (;;) {
            unsigned long long wait = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                unsigned long long t = now();
                if (globalUntil_ > t) wait = globalUntil_ - t;
                if (auto rb = routeBucket_.find(key); rb != routeBucket_.end()) {
                    Bucket& b = buckets_[rb->second];
                    if (b.remaining <= 0 && b.resetAt > t)
                        wait = std::max(wait, b.resetAt - t);
                    else if (b.resetAt <= t)
                        b.remaining = 1;
                    if (wait == 0) b.remaining--;  // claim a slot before others take it
                }
            }
            if (wait == 0) break;
            Sleep(DWORD(std::min<unsigned long long>(wait + 50, 60000)));
        }
        r = http_.Send(req);
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::string hash = r.header("x-ratelimit-bucket");
            if (!hash.empty()) {
                routeBucket_[key] = hash;
                Bucket& b = buckets_[hash];
                std::string rem = r.header("x-ratelimit-remaining"), after = r.header("x-ratelimit-reset-after");
                if (!rem.empty()) b.remaining = atoi(rem.c_str());
                if (!after.empty()) b.resetAt = now() + (unsigned long long)(atof(after.c_str()) * 1000);
            }
        }
        if (r.status != 429) return r;
        double retry = 1;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_object() && j.contains("retry_after") && j["retry_after"].is_number()) retry = j["retry_after"].get<double>();
        bool global = (j.is_object() && j.value("global", false)) || r.header("x-ratelimit-global") == "true";
        bool known = true;
        {
            std::lock_guard<std::mutex> lock(mu_);
            unsigned long long until = now() + (unsigned long long)(retry * 1000) + 100;
            if (global)
                globalUntil_ = std::max(globalUntil_, until);
            else if (auto rb = routeBucket_.find(key); rb != routeBucket_.end())
                buckets_[rb->second] = {0, until};
            else
                known = false;
        }
        // Without a bucket to wait on, wait here.
        if (!known) Sleep(DWORD(std::min(retry * 1000 + 100, 60000.0)));
    }
    return r;
}

HttpResult Api::Call(const std::string& method, const std::string& path, const json& body) {
    HttpRequest req;
    req.method = method;
    req.url = std::string(kApi) + path;
    req.headers = headers(true);
    if (!body.is_null()) {
        req.body = body.dump();
        req.contentType = "application/json";
    }
    return send(std::move(req), route(method, path));
}

HttpResult Api::Upload(const std::string& path, const json& payload, const std::vector<File>& files) {
    Multipart mp;
    mp.Field("payload_json", payload.dump(), "application/json");
    for (size_t i = 0; i < files.size(); i++)
        mp.File("files[" + std::to_string(i) + "]", files[i].name, files[i].bytes, files[i].type);
    HttpRequest req;
    req.method = "POST";
    req.url = std::string(kApi) + path;
    req.headers = headers(true);
    req.body = mp.Body();
    req.contentType = mp.ContentType();
    return send(std::move(req), route("POST", path));
}

HttpResult Api::Unauthed(const std::string& method, const std::string& path, const json& body) {
    HttpRequest req;
    req.method = method;
    req.url = std::string(kApi) + path;
    req.headers = headers(false);
    if (!body.is_null()) {
        req.body = body.dump();
        req.contentType = "application/json";
    }
    return send(std::move(req), route(method, path));
}

// --- worker ---

Worker::Worker()
    : thread_([this] {
          CoInitializeEx(nullptr, COINIT_MULTITHREADED);
          for (;;) {
              std::function<void()> job;
              {
                  std::unique_lock<std::mutex> lock(mu_);
                  cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
                  if (stop_) break;
                  job = std::move(jobs_.front());
                  jobs_.pop_front();
              }
              job();
          }
          CoUninitialize();
      }) {}

Worker::~Worker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Worker::Run(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

// --- gateway ---

Gateway::Gateway(std::function<void()> wake) : wake_(std::move(wake)) {}

Gateway::~Gateway() { Stop(); }

void Gateway::Start(const Identity& id, const std::string& token) {
    Stop();
    id_ = id;
    token_ = token;
    stop_ = false;
    sessionId_.clear();
    resumeUrl_.clear();
    seq_ = 0;
    thread_ = std::thread(&Gateway::run, this);
}

void Gateway::Stop() {
    stop_ = true;
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(wsMu_);
        if (ws_) ws_->Close();
    }
    if (thread_.joinable()) thread_.join();
    state_ = GatewayState::Idle;
}

bool Gateway::Send(int op, const json& d) {
    std::shared_ptr<WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(wsMu_);
        ws = ws_;
    }
    return ws && ws->Send(json{{"op", op}, {"d", d}}.dump());
}

void Gateway::Drain(std::vector<Dispatch>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& d : inbox_) out.push_back(std::move(d));
    inbox_.clear();
}

std::string Gateway::LastError() {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void Gateway::push(Dispatch d) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        inbox_.push_back(std::move(d));
    }
    wake_();
}

void Gateway::run() {
    int backoff = 1;
    std::mt19937 rng(std::random_device{}());
    while (!stop_) {
        state_ = GatewayState::Connecting;
        std::string error;
        USHORT close = 0;
        unsigned long long started = now();
        bool reached = session(error, close);
        {
            std::lock_guard<std::mutex> lock(wsMu_);
            ws_.reset();
        }
        if (stop_) break;
        {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = error;
        }
        if (close == 4004) {  // authentication failed: the token's no good
            state_ = GatewayState::AuthFailed;
            push({"", {{"auth_failed", true}}});
            return;
        }
        // These can't be resumed: start a new session.
        if (close == 4007 || close == 4009 || (close >= 4010 && close <= 4014)) sessionId_.clear();
        state_ = GatewayState::Connecting;
        push({"", {{"disconnected", error}}});
        if (reached && now() - started > 30000) backoff = 1;
        // A dropped session resumes at once the first time; after that,
        // back off with a little jitter.
        int wait = reached && backoff == 1 ? 0 : backoff * 1000 + int(rng() % 1000);
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(wait), [&] { return stop_.load(); });
        backoff = std::min(backoff * 2, 60);
    }
}

bool Gateway::session(std::string& error, USHORT& close) {
    auto ws = std::make_shared<WebSocket>(id_.userAgent);
    bool resuming = !sessionId_.empty() && !resumeUrl_.empty();
    std::string url = (resuming ? resumeUrl_ : std::string("wss://gateway.discord.gg")) + "/?encoding=json&v=9";
    Headers h = {{"Origin", "https://discord.com"}, {"Accept-Language", "en-GB,en;q=0.9"}, {"Cache-Control", "no-cache"},
                 {"Pragma", "no-cache"}};
    if (!ws->Connect(url, h, error)) return false;
    {
        std::lock_guard<std::mutex> lock(wsMu_);
        if (stop_) return false;
        ws_ = ws;
    }

    std::string msg;
    if (!ws->Receive(msg, error, &close)) return false;
    json hello = json::parse(msg, nullptr, false);
    if (!hello.is_object() || hello.value("op", -1) != 10) {
        error = "no hello from the gateway";
        return false;
    }
    int interval = hello["d"].value("heartbeat_interval", 41250);

    // Heartbeats, on their own thread. A heartbeat that isn't acknowledged
    // by the next one means the connection's dead: drop it and resume.
    std::mutex hbMu;
    std::condition_variable hbCv;
    bool ended = false;
    acked_ = true;
    std::thread heartbeat([&] {
        std::mt19937 rng(std::random_device{}());
        int first = int(interval * (std::uniform_real_distribution<double>(0.0, 1.0)(rng)));
        int wait = first;
        for (;;) {
            std::unique_lock<std::mutex> lock(hbMu);
            if (hbCv.wait_for(lock, std::chrono::milliseconds(wait), [&] { return ended || stop_.load(); })) return;
            lock.unlock();
            if (!acked_) {
                ws->Abort();
                return;
            }
            acked_ = false;
            int64_t s = seq_;
            ws->Send(json{{"op", 1}, {"d", s ? json(s) : json(nullptr)}}.dump());
            wait = interval;
        }
    });
    auto finish = [&] {
        {
            std::lock_guard<std::mutex> lock(hbMu);
            ended = true;
        }
        hbCv.notify_all();
        heartbeat.join();
    };

    if (resuming) {
        ws->Send(json{{"op", 6}, {"d", {{"token", token_}, {"session_id", sessionId_}, {"seq", seq_.load()}}}}.dump());
    } else {
        seq_ = 0;
        // Capabilities: lazy notes, no affine IDs, versioned read states
        // and guild settings, deduplicated users, prioritised READY,
        // non-channel read states, settings as protobuf, client state v2
        // and debounced reactions.
        int capabilities = 1 | 2 | 4 | 8 | 16 | 32 | 128 | 512 | 1024 | 8192;
        json identify = {
            {"token", token_},
            {"capabilities", capabilities},
            {"properties", id_.properties},
            {"presence", {{"status", "unknown"}, {"since", 0}, {"activities", json::array()}, {"afk", false}}},
            {"compress", false},
            {"client_state", {{"guild_versions", json::object()}}},
        };
        ws->Send(json{{"op", 2}, {"d", identify}}.dump());
    }

    bool reached = false;
    for (;;) {
        if (!ws->Receive(msg, error, &close)) break;
        json j = json::parse(msg, nullptr, false);
        if (!j.is_object()) continue;
        int op = j.value("op", -1);
        if (op == 0) {
            if (j.contains("s") && j["s"].is_number()) seq_ = j["s"].get<int64_t>();
            std::string type = j.value("t", "");
            if (type == "READY") {
                const json& d = j["d"];
                sessionId_ = d.value("session_id", "");
                resumeUrl_ = d.value("resume_gateway_url", "");
            }
            if (type == "READY" || type == "RESUMED") {
                reached = true;
                state_ = GatewayState::Connected;
            }
            push({std::move(type), std::move(j["d"])});
        } else if (op == 1) {
            ws->Send(json{{"op", 1}, {"d", seq_.load()}}.dump());
        } else if (op == 7) {  // reconnect, and resume
            error = "the gateway asked to reconnect";
            break;
        } else if (op == 9) {  // invalid session
            if (!(j["d"].is_boolean() && j["d"].get<bool>())) sessionId_.clear();
            error = "invalid session";
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(1000 + rand() % 4000), [&] { return stop_.load(); });
            break;
        } else if (op == 11) {
            acked_ = true;
        }
    }
    ws->Abort();
    finish();
    return reached;
}

}  // namespace disky
