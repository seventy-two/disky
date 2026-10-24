#include "rtp.h"

#include <algorithm>
#include <cstring>

namespace disky {

namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put16(std::string& s, uint16_t v) {
    s += char(v >> 8);
    s += char(v & 255);
}
void put32(std::string& s, uint32_t v) {
    put16(s, uint16_t(v >> 16));
    put16(s, uint16_t(v & 0xFFFF));
}

}  // namespace

bool ParseRtp(const uint8_t* p, size_t n, Rtp* r) {
    if (n < 12 || p[0] >> 6 != 2) return false;
    r->padding = p[0] & 0x20;
    r->extension = p[0] & 0x10;
    int csrcs = p[0] & 0x0F;
    r->payloadType = p[1] & 0x7F;
    r->seq = be16(p + 2);
    r->timestamp = be32(p + 4);
    r->ssrc = be32(p + 8);
    r->header = 12 + 4 * size_t(csrcs);
    r->extensionBytes = 0;
    if (r->extension) {
        if (n < r->header + 4) return false;
        r->extensionBytes = 4 * size_t(be16(p + r->header + 2));
        r->header += 4;
    }
    return n >= r->header;
}

bool SplitSealed(const Rtp& r, size_t n, const uint8_t* p, Sealed* s) {
    if (n < r.header + 16 + 4) return false;
    s->cipher = r.header;
    s->cipherLen = n - r.header - 16 - 4;
    s->tag = n - 16 - 4;
    std::memset(s->nonce, 0, sizeof(s->nonce));
    std::memcpy(s->nonce, p + n - 4, 4);
    return true;
}

bool Unwrap(const Rtp& r, std::string& plain) {
    if (plain.size() < r.extensionBytes) return false;
    plain.erase(0, r.extensionBytes);
    if (r.padding) {
        if (plain.empty()) return false;
        size_t pad = uint8_t(plain.back());
        if (pad == 0 || pad > plain.size()) return false;
        plain.resize(plain.size() - pad);
    }
    return true;
}

std::string RtpHeader(uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
    std::string h;
    h += char(0x80);
    h += char(kOpusPayloadType);
    put16(h, seq);
    put32(h, timestamp);
    put32(h, ssrc);
    return h;
}

std::string DiscoveryRequest(uint32_t ssrc) {
    std::string s;
    put16(s, 1);   // request
    put16(s, 70);  // length after these 4 bytes
    put32(s, ssrc);
    s.resize(74, '\0');  // 64 bytes of address and 2 of port, empty
    return s;
}

bool ParseDiscovery(const uint8_t* p, size_t n, std::string* ip, uint16_t* port) {
    if (n < 74 || be16(p) != 2) return false;
    const char* a = reinterpret_cast<const char*>(p + 8);
    *ip = std::string(a, strnlen(a, 64));
    *port = be16(p + 72);
    return !ip->empty();
}

bool ParseVoiceBinary(const std::string& msg, int* seq, int* op, std::string* payload) {
    if (msg.size() < 3) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(msg.data());
    *seq = be16(p);
    *op = p[2];
    *payload = msg.substr(3);
    return true;
}

std::string VoiceBinary(int op, const std::string& payload) { return std::string(1, char(op)) + payload; }

int Playout::Arrived(uint16_t seq) {
    if (!started_) {
        started_ = true;
        last_ = seq;
        return 0;
    }
    int16_t gap = int16_t(uint16_t(seq - last_));
    if (gap <= 0) return -1;
    last_ = seq;
    // A long gap is a pause or a restart, not loss worth filling in.
    return gap - 1 <= 5 ? gap - 1 : 0;
}

void Playout::Push(const float* samples, size_t frames) {
    pcm_.insert(pcm_.end(), samples, samples + frames * kChannels);
    if (Buffered() > kPlayoutMax) {
        size_t drop = (Buffered() - kPlayoutTarget) * kChannels;
        pcm_.erase(pcm_.begin(), pcm_.begin() + drop);
    }
}

size_t Playout::Mix(float* out, size_t frames, float gain) {
    if (filling_) {
        if (Buffered() < kPlayoutTarget) return 0;
        filling_ = false;
    }
    size_t n = std::min(frames, Buffered());
    for (size_t i = 0; i < n * kChannels; i++) out[i] += pcm_[i] * gain;
    pcm_.erase(pcm_.begin(), pcm_.begin() + n * kChannels);
    // Run dry, and it waits to fill again before playing on.
    if (pcm_.empty()) filling_ = true;
    return n;
}

void Playout::Reset() {
    pcm_.clear();
    started_ = false;
    filling_ = true;
}

}  // namespace disky
