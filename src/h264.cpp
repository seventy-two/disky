#include "h264.h"

namespace disky {

namespace {

const char kStart[] = {0, 0, 0, 1};

}  // namespace

bool H264Assembler::build(const std::map<uint16_t, std::string>& packets, std::string* out) const {
    out->clear();
    bool inFu = false;
    // Iterate in sequence order from base_, so a wrap past 65535 keeps order.
    for (uint16_t i = 0; i < packets.size(); i++) {
        auto it = packets.find(uint16_t(base_ + i));
        if (it == packets.end()) return false;
        const std::string& p = it->second;
        if (p.empty()) return false;
        int type = p[0] & 0x1F;
        if (type >= 1 && type <= 23) {
            out->append(kStart, 4);
            out->append(p);
        } else if (type == 24) {  // STAP-A: sizes and NAL units
            size_t at = 1;
            while (at + 2 <= p.size()) {
                size_t n = size_t(uint8_t(p[at])) << 8 | uint8_t(p[at + 1]);
                at += 2;
                if (n == 0 || at + n > p.size()) return false;
                out->append(kStart, 4);
                out->append(p, at, n);
                at += n;
            }
        } else if (type == 28) {  // FU-A: one NAL unit in pieces
            if (p.size() < 2) return false;
            bool start = p[1] & 0x80, end = p[1] & 0x40;
            if (start) {
                out->append(kStart, 4);
                out->push_back(char((p[0] & 0xE0) | (p[1] & 0x1F)));
                inFu = true;
            } else if (!inFu) {
                return false;
            }
            out->append(p, 2, std::string::npos);
            if (end) inFu = false;
        } else {
            return false;
        }
    }
    return !out->empty() && !inFu;
}

bool H264Assembler::Packet(uint16_t seq, uint32_t timestamp, bool marker, const std::string& payload, std::string* out,
                           bool* lost) {
    *lost = false;
    if (!have_ || timestamp != timestamp_) {
        // A new frame: the one before never finished, so it's lost.
        if (have_ && !packets_.empty()) {
            *lost = true;
            waitingKey_ = true;
        }
        // A packet from a frame already done or dropped: too late.
        if (have_ && int32_t(timestamp - timestamp_) < 0) return false;
        have_ = true;
        timestamp_ = timestamp;
        packets_.clear();
        base_ = seq;
    }
    if (int16_t(uint16_t(seq - base_)) < 0) base_ = seq;  // an earlier packet of this frame
    packets_[seq] = payload;
    // Complete once the marker's in and the run from base_ has no holes.
    uint16_t lastSeq = 0;
    bool haveMarker = false;
    if (marker) {
        haveMarker = true;
        lastSeq = seq;
        markerSeq_ = seq;
        markerSeen_ = true;
    } else if (markerSeen_) {
        haveMarker = true;
        lastSeq = markerSeq_;
    }
    if (!haveMarker) return false;
    if (uint16_t(lastSeq - base_) + 1u != packets_.size()) return false;
    // A gap since the last frame means its first packets went missing.
    if (lastValid_ && uint16_t(base_ - last_) != 1) {
        *lost = true;
        waitingKey_ = true;
    }
    std::string frame;
    bool ok = build(packets_, &frame);
    lastValid_ = true;
    last_ = lastSeq;
    packets_.clear();
    markerSeen_ = false;
    if (!ok) {
        *lost = true;
        waitingKey_ = true;
        return false;
    }
    if (waitingKey_) {
        if (!KeyFrame(frame)) return false;
        waitingKey_ = false;
    }
    *out = std::move(frame);
    return true;
}

bool H264Assembler::KeyFrame(const std::string& f) {
    for (size_t i = 0; i + 4 < f.size(); i++)
        if (f[i] == 0 && f[i + 1] == 0 && f[i + 2] == 0 && f[i + 3] == 1 && (f[i + 4] & 0x1F) == 5) return true;
    return false;
}

}  // namespace disky
