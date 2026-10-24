// A voice channel connection: the voice gateway, audio over UDP, Discord's
// end-to-end encryption (DAVE), playing what everyone says, and sending
// what the microphone hears.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "store.h"

namespace disky {

class VoiceConnection {
public:
    struct Params {
        Snowflake userId = 0, guildId = 0, channelId = 0;
        std::string sessionId, token, endpoint;
        std::string userAgent;
        std::string output;  // the output device's name; empty for Windows' default
    };
    // wake is called from the connection's threads when something's
    // changed: its state, or who's speaking.
    VoiceConnection(Params p, std::function<void()> wake);
    ~VoiceConnection();
    VoiceConnection(const VoiceConnection&) = delete;
    VoiceConnection& operator=(const VoiceConnection&) = delete;

    enum class State { Connecting, Connected, Failed };
    State GetState() const;
    // Error is why it failed, as a sentence.
    std::string Error() const;
    // Ping is the voice server's round trip in ms; 0 until measured.
    int Ping() const;
    // Encrypted says the call's end-to-end encrypted.
    bool Encrypted() const;

    // Speaking says whether someone's been heard in the last moment.
    bool Speaking(Snowflake user) const;
    bool AnySpeaking() const;
    // SetVolume sets one person's volume, 1 being as sent (up to 2).
    void SetVolume(Snowflake user, float gain);
    // SetDeafened stops (or restarts) playing anything.
    void SetDeafened(bool deaf);
    // SetOutputVolume sets everything's volume, 0 to 1.
    void SetOutputVolume(float gain);

    // Sending: off (muted), when the mic's louder than a threshold in dBFS,
    // or while the push-to-talk key's held.
    enum class Send { Off, VoiceActivity, PushToTalk };
    void SetSending(Send mode, float threshold);
    void SetPushToTalk(bool held);
    // PushMic takes 20ms of mono audio from the microphone's thread.
    void PushMic(const float* samples, size_t n);
    // Transmitting says we're sending sound now.
    bool Transmitting() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace disky
