#pragma once
#include <cstddef>

// Transport-layer timing and sizing constants shared by every USB backend.
//
// Protocol-level windows (handshake drain, write-ack drain, credit drain) are
// runtime options that live in ExecutorOptions / SessionOptions; the constants
// here are the raw-I/O knobs underneath them. One definition so a value tuned
// on one platform cannot silently go stale on the other.

namespace ewr::usb_timing {

    // Hard budget for one bulk-OUT write to complete.
    inline constexpr int kWriteTimeoutMs = 2000;

    // Extra silence tolerated inside an already-flowing reply burst: once the
    // first bytes have arrived, a pause this long means the burst is over.
    inline constexpr int kDrainFollowUpTimeoutMs = 250;

    // Windows only: usbprint.sys can complete a read with 0 bytes when the
    // reverse channel is momentarily empty instead of pending until data
    // arrives, so the drain loop re-posts the read at this interval until the
    // deadline passes.
    inline constexpr int kDrainPollIntervalMs = 10;

    // One drain call never collects more than this many bytes...
    inline constexpr std::size_t kMaxDrainBytes = 64 * 1024;

    // ...nor accepts more than this many non-empty reads.
    inline constexpr int kMaxDrainReads = 64;

    // Chunk size for a single bulk-IN read.
    inline constexpr std::size_t kDrainReadChunkBytes = 256;

    // Windows: completion budget for IOCTL_USBPRINT_SOFT_RESET.
    inline constexpr int kSoftResetTimeoutMs = 1000;

    // Linux: budget for the IEEE 1284 GET_DEVICE_ID control transfer.
    inline constexpr int kDeviceIdTimeoutMs = 1000;

} // namespace ewr::usb_timing
