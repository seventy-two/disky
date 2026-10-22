// Talking to Discord: who the client says it is, the REST API with its
// rate limits, and the gateway's event stream.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net.h"
#include "store.h"

namespace disky {

constexpr const char* kApi = "https://discord.com/api/v9";

// Identity is how the client presents itself: as Discord's web app in
// Microsoft Edge, the browser on this PC, so its traffic looks like any
// other web session's.
struct Identity {
    std::string userAgent;
    std::string browserVersion;
    int buildNumber = 0;
    json properties;          // the gateway's identify properties
    std::string superProps;   // the same, base64, for X-Super-Properties
    std::string locale = "en-GB";
    std::string timezone = "Europe/London";
};

// MakeIdentity builds the identity for a client build number.
Identity MakeIdentity(int buildNumber);
// FetchBuildNumber reads the web app's current build number from its
// page; 0 if it can't be found.
int FetchBuildNumber(Http& http);

// Api makes REST calls as the user. It keeps to Discord's rate limits:
// it waits out a bucket that's used up, and retries after a 429.
class Api {
public:
    Api(Identity id, std::string token);
    HttpResult Call(const std::string& method, const std::string& path, const json& body = nullptr);
    // Upload sends a message with files, as multipart form data.
    struct File {
        std::string name, bytes, type;
    };
    HttpResult Upload(const std::string& path, const json& payload, const std::vector<File>& files);
    // Plain makes a request without the token, e.g. for the QR login.
    HttpResult Unauthed(const std::string& method, const std::string& path, const json& body);
    const Identity& identity() const { return id_; }

private:
    HttpResult send(HttpRequest req, const std::string& route);
    Headers headers(bool auth) const;
    Identity id_;
    std::string token_;
    Http http_;
    std::mutex mu_;
    struct Bucket {
        int remaining = 1;
        unsigned long long resetAt = 0;  // GetTickCount64
    };
    std::unordered_map<std::string, std::string> routeBucket_;  // route → bucket hash
    std::unordered_map<std::string, Bucket> buckets_;
    unsigned long long globalUntil_ = 0;
};

// Worker runs jobs in order on its own thread.
class Worker {
public:
    Worker();
    ~Worker();
    void Run(std::function<void()> job);

private:
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
};

struct Dispatch {
    std::string type;  // the event's name, or "" for connection news
    json data;
};

enum class GatewayState { Idle, Connecting, Connected, AuthFailed };

// Gateway keeps the event stream open on its own thread: identifying,
// heartbeating, resuming after drops, and reconnecting with backoff.
class Gateway {
public:
    // wake is called from the gateway's thread when there's something to Drain.
    explicit Gateway(std::function<void()> wake);
    ~Gateway();
    void Start(const Identity& id, const std::string& token);
    void Stop();
    // Send sends an op now, if connected; ops sent while not are dropped,
    // as READY starts over anyway.
    bool Send(int op, const json& d);
    void Drain(std::vector<Dispatch>& out);
    GatewayState State() const { return state_; }
    std::string LastError();

private:
    void run();
    bool session(std::string& error, USHORT& close);
    void push(Dispatch d);

    std::function<void()> wake_;
    Identity id_;
    std::string token_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<GatewayState> state_{GatewayState::Idle};

    std::mutex wsMu_;
    std::shared_ptr<WebSocket> ws_;

    // Resuming.
    std::string sessionId_, resumeUrl_;
    std::atomic<int64_t> seq_{0};
    std::atomic<bool> acked_{true};

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Dispatch> inbox_;
    std::string error_;
};

// NewNonce makes a nonce for a message, as Discord's client does: a
// snowflake for now.
std::string NewNonce();

}  // namespace disky
