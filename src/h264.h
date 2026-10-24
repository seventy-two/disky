// Putting H.264 video back together from RTP packets (RFC 6184): single
// NAL units, STAP-A and FU-A, into whole frames as Annex B, each NAL unit
// after a 4-byte start code (the form libdave encrypts frames in).
// Portable, so it's tested on Linux too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace disky {

constexpr int kH264PayloadType = 101;

class H264Assembler {
public:
    // Packet takes one packet's payload. When it completes a frame (the
    // marker bit's set and nothing's missing), it returns true with the
    // frame in out. lost is set when packets went missing, so a key frame
    // should be asked for; frames after a loss are held back until one
    // arrives.
    bool Packet(uint16_t seq, uint32_t timestamp, bool marker, const std::string& payload, std::string* out, bool* lost);

    // KeyFrame says whether a frame holds an IDR slice.
    static bool KeyFrame(const std::string& annexB);

private:
    bool build(const std::map<uint16_t, std::string>& packets, std::string* out) const;

    uint32_t timestamp_ = 0;
    bool have_ = false;
    std::map<uint16_t, std::string> packets_;  // this frame's, by sequence (wrap handled by base)
    uint16_t base_ = 0;                        // the frame's first sequence number seen
    bool lastValid_ = false;
    uint16_t last_ = 0;                        // the last frame's final sequence number
    bool waitingKey_ = true;                   // until a key frame, nothing's decodable
    bool markerSeen_ = false;                  // this frame's last packet's in
    uint16_t markerSeq_ = 0;
};

}  // namespace disky
