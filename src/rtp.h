// Voice's packets, apart from the encryption itself: RTP as Discord sends
// it, the UDP address discovery, the voice gateway's binary messages, and
// evening out when audio arrives. Portable, so it's tested on Linux too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace disky {

constexpr int kOpusPayloadType = 0x78;
constexpr int kSampleRate = 48000, kChannels = 2;
constexpr int kFrameSamples = 960;  // 20ms, per channel

// Rtp is a packet's header.
struct Rtp {
    uint8_t payloadType = 0;
    bool padding = false, extension = false;
    uint16_t seq = 0;
    uint32_t timestamp = 0, ssrc = 0;
    // header is what's sent in the clear but authenticated: the fixed
    // header, any CSRCs and, with an extension, its 4-byte header. The
    // extension's body is encrypted along with the audio.
    size_t header = 0;
    size_t extensionBytes = 0;
};

// ParseRtp reads a packet's header; false if it isn't an RTP version 2
// packet long enough to hold one.
bool ParseRtp(const uint8_t* p, size_t n, Rtp* out);

// Sealed is where an aead_*_rtpsize packet's parts are: the header (the
// additional data), ciphertext, the 16-byte tag, and the 4-byte nonce
// counter at the end, which is the first 4 bytes of a 12-byte nonce.
struct Sealed {
    size_t cipher = 0, cipherLen = 0, tag = 0;
    uint8_t nonce[12] = {};
};
bool SplitSealed(const Rtp& r, size_t n, const uint8_t* p, Sealed* out);

// Unwrap takes a decrypted payload down to the audio: the extension's body
// off the front and, with padding, the padding off the end. False if it
// doesn't fit.
bool Unwrap(const Rtp& r, std::string& plain);

// RtpHeader is the 12-byte header for an Opus packet we send.
std::string RtpHeader(uint16_t seq, uint32_t timestamp, uint32_t ssrc);

// IP discovery: a request for our address as the voice server sees it,
// and reading its answer.
std::string DiscoveryRequest(uint32_t ssrc);
bool ParseDiscovery(const uint8_t* p, size_t n, std::string* ip, uint16_t* port);

// The voice gateway's binary messages: from the server, a 2-byte sequence
// number and a 1-byte opcode before the payload; to it, just the opcode.
bool ParseVoiceBinary(const std::string& msg, int* seq, int* op, std::string* payload);
std::string VoiceBinary(int op, const std::string& payload);

// Playout is one speaker's decoded audio, waiting to be played. It holds a
// little back so packets arriving unevenly still play smoothly, and drops
// what's too far behind.
class Playout {
public:
    // Arrived says what to do with a packet by its sequence number: how
    // many frames were lost before it (to conceal), or -1 if it's late or
    // a repeat and should be dropped.
    int Arrived(uint16_t seq);
    // Push adds decoded audio, interleaved stereo.
    void Push(const float* samples, size_t frames);
    // Mix adds frames of audio into out at gain, as far as there is any;
    // it returns how many it added.
    size_t Mix(float* out, size_t frames, float gain);
    size_t Buffered() const { return pcm_.size() / kChannels; }
    void Reset();

private:
    std::deque<float> pcm_;
    bool started_ = false, filling_ = true;
    uint16_t last_ = 0;
};

constexpr size_t kPlayoutTarget = kSampleRate * 60 / 1000;  // held back before playing
constexpr size_t kPlayoutMax = kSampleRate * 250 / 1000;    // more than this and it skips ahead

}  // namespace disky
