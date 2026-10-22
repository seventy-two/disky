// Signing in with a QR code, as Discord's own login page does: the phone
// app scans it, the user confirms there, and the token arrives encrypted
// to a key made for this one sign-in.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "discord.h"

namespace disky {

class RemoteAuth {
public:
    struct Status {
        enum class Stage { Connecting, Waiting, Scanned, Done, Failed } stage = Stage::Connecting;
        std::string qr;  // what the QR code says, while Waiting
        std::string username, userId, avatar;  // who scanned it
        std::string token;  // when Done
        std::string error;  // when Failed
    };

    RemoteAuth(Identity id, std::function<void()> wake);
    ~RemoteAuth();
    // Start begins a sign-in, or starts over with a new code.
    void Start();
    void Stop();
    Status Get();

private:
    void run();
    bool attempt(std::string& error);
    void set(const std::function<void(Status&)>& f);

    Identity id_;
    std::function<void()> wake_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex wsMu_;
    std::shared_ptr<WebSocket> ws_;
    std::mutex mu_;
    Status status_;
};

}  // namespace disky
