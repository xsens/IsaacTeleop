// SPDX-FileCopyrightText: Copyright (c) 2026 Xsens Technologies B.V. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "xsens_full_body_plugin.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace plugins::xsens_full_body;

namespace
{

constexpr std::string_view DEFAULT_COLLECTION_ID = "xsens_full_body";
constexpr uint16_t DEFAULT_UDP_PORT = 9764;
//! Must match the reader's `max_flatbuffer_size`. A mismatch throws loudly on both sides -- which
//! is the good case; a `collection_id` mismatch instead fails silently and forever.
constexpr size_t DEFAULT_MAX_FLATBUFFER_SIZE = 4096;
constexpr uint64_t STATS_EVERY = 250;

//! Atomic rather than volatile sig_atomic_t: the plugin observes this from inside its recovery
//! backoffs, so a stop request lands during an outage instead of waiting the budget out.
std::atomic<bool> g_stop{ false };

extern "C" void handle_signal(int)
{
    g_stop.store(true, std::memory_order_relaxed);
}

bool parse_size(std::string_view text, size_t& out)
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, out);
    return error == std::errc{} && ptr == end && out > 0;
}

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " [collection_id] [udp_port] [max_flatbuffer_size]\n\n"
              << "Receives Xsens MVN Studio's Isaac Teleop UDP stream and republishes it as an\n"
              << "Isaac Teleop tensor collection, readable via the `body.xsens` vendor.\n\n"
              << "Defaults: " << DEFAULT_COLLECTION_ID << " " << DEFAULT_UDP_PORT << " " << DEFAULT_MAX_FLATBUFFER_SIZE
              << "  (matching MVN's \"Isaac Teleop\" preset)\n";
}

//! Every counter, on one line. Printed periodically and once more on exit -- including the
//! unrecoverable-outage exit, so a pusher that gave up still says what it saw first.
void print_stats(const XsensFullBodyStats& s, const char* prefix)
{
    std::cout << prefix << "delivered=" << s.delivered << " seq=" << s.last_seq << " size=" << s.last_size
              << " fnv1a64=0x" << std::hex << s.last_hash << std::dec << " malformed=" << s.dropped_malformed
              << " unverified=" << s.dropped_unverified << " stale=" << s.dropped_stale
              << " truncated=" << s.dropped_truncated << " gapEvents=" << s.sequence_gap_events
              << " seqSkipped=" << s.sequence_numbers_skipped << " resets=" << s.session_resets
              << " rewinds=" << s.timeline_rewinds << " nonWholeMs=" << s.non_whole_ms_samples
              << " socketRecoveries=" << s.socket_recoveries << " socketRecoveryFailures=" << s.socket_recovery_failures
              << " pushFailures=" << s.push_failures << " sessionRecoveries=" << s.session_recoveries << std::endl;
}

} // namespace

int main(int argc, char** argv)
try
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        usage(argv[0]);
        return 0;
    }
    if (argc > 4)
    {
        usage(argv[0]);
        return 1;
    }

    const std::string collection_id = (argc > 1) ? argv[1] : std::string(DEFAULT_COLLECTION_ID);

    uint16_t udp_port = DEFAULT_UDP_PORT;
    if (argc > 2)
    {
        size_t parsed = 0;
        if (!parse_size(argv[2], parsed) || parsed > 65535)
        {
            std::cerr << argv[0] << ": invalid udp_port '" << argv[2] << "'" << std::endl;
            return 1;
        }
        udp_port = static_cast<uint16_t>(parsed);
    }

    size_t max_flatbuffer_size = DEFAULT_MAX_FLATBUFFER_SIZE;
    if (argc > 3 && !parse_size(argv[3], max_flatbuffer_size))
    {
        std::cerr << argv[0] << ": invalid max_flatbuffer_size '" << argv[3] << "'" << std::endl;
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "Xsens Full Body Pusher (collection: " << collection_id << ", tensor: full_body_pose"
              << ", udp: 0.0.0.0:" << udp_port << ", max_flatbuffer_size: " << max_flatbuffer_size << ")" << std::endl;

    XsensFullBodyPlugin plugin(collection_id, udp_port, max_flatbuffer_size);

    std::cout << "listening on 0.0.0.0:" << udp_port << " -> push_buffer" << std::endl;
    std::cout << "In MVN Studio: Options -> Network Streamer -> preset \"Isaac Teleop\", tick the row, press Play."
              << std::endl;

    // A broken socket or a restarted CloudXR runtime is recovered inside update(). It only throws
    // once a retry budget is exhausted, which is the operator-restart case -- and even then the
    // counters go out, because they are usually the only record of what led up to it.
    int exit_code = 0;
    uint64_t since_stats = 0;
    try
    {
        while (!g_stop.load(std::memory_order_relaxed))
        {
            if (!plugin.update(g_stop))
            {
                continue;
            }
            if (++since_stats >= STATS_EVERY)
            {
                since_stats = 0;
                print_stats(plugin.stats(), "[XsensFullBody] ");
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n" << argv[0] << ": " << e.what() << std::endl;
        exit_code = 1;
    }

    print_stats(plugin.stats(), "\nstopped: ");
    return exit_code;
}
catch (const std::exception& e)
{
    std::cerr << argv[0] << ": " << e.what() << std::endl;
    return 1;
}
catch (...)
{
    std::cerr << argv[0] << ": Unknown error" << std::endl;
    return 1;
}
