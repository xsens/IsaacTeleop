// SPDX-FileCopyrightText: Copyright (c) 2026 Xsens Technologies B.V. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "xsens_full_body_plugin.hpp"

#include "teleop_wire.hpp"

#include <arpa/inet.h>
#include <flatbuffers/flatbuffers.h>
#include <netinet/in.h>
#include <oxr/oxr_session.hpp>
#include <oxr_utils/os_time.hpp>
#include <schema/full_body_generated.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace plugins
{
namespace xsens_full_body
{

namespace
{

constexpr std::string_view TENSOR_IDENTIFIER = "full_body_pose";
//! Bounded so a stalled MVN cannot wedge the loop; the caller just gets a false from update().
constexpr int RECV_TIMEOUT_MS = 250;
//! MVN's datagram is 820 B. Anything wildly larger is not ours; cap the read buffer generously.
constexpr size_t RECV_BUFFER_SIZE = 64 * 1024;
constexpr int64_t NS_PER_MS = 1000000;

/*!
 * @brief Full FlatBuffers bounds check on a payload before it is trusted.
 *
 * Mandatory, and easy to skip by accident: `deserialize_teleop_frame` validates *framing* only
 * and never looks inside the payload. It also cannot use a generated `VerifyFullBodyPoseBuffer`,
 * because the schema's `root_type` is `FullBodyPoseRecord` and flatc emits no verifier for a
 * non-root table -- hence `VerifyBuffer<FullBodyPose>` by hand.
 */
bool verify_full_body_payload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }
    flatbuffers::Verifier verifier(data, size);
    return verifier.VerifyBuffer<core::FullBodyPose>(nullptr);
}

} // namespace

XsensFullBodyPlugin::XsensFullBodyPlugin(const std::string& collection_id, uint16_t udp_port, size_t max_flatbuffer_size)
    : buffer_(RECV_BUFFER_SIZE),
      session_(
          std::make_shared<core::OpenXRSession>("XsensFullBodyPlugin", core::SchemaPusher::get_required_extensions())),
      pusher_(session_->get_handles(),
              core::SchemaPusherConfig{ .collection_id = collection_id,
                                        .max_flatbuffer_size = max_flatbuffer_size,
                                        .tensor_identifier = std::string(TENSOR_IDENTIFIER),
                                        .localized_name = "Xsens Full Body",
                                        .app_name = "XsensFullBodyPlugin" })
{
    open_socket(udp_port);
}

XsensFullBodyPlugin::~XsensFullBodyPlugin()
{
    close_socket();
}

void XsensFullBodyPlugin::open_socket(uint16_t port)
{
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0)
    {
        throw std::runtime_error(std::string("XsensFullBodyPlugin: socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        close_socket();
        throw std::runtime_error(std::string("XsensFullBodyPlugin: SO_REUSEADDR failed: ") + std::strerror(errno));
    }

    timeval timeout{};
    timeout.tv_sec = RECV_TIMEOUT_MS / 1000;
    timeout.tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        close_socket();
        throw std::runtime_error(std::string("XsensFullBodyPlugin: SO_RCVTIMEO failed: ") + std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        const std::string reason = std::strerror(errno);
        close_socket();
        throw std::runtime_error("XsensFullBodyPlugin: bind to UDP port " + std::to_string(port) +
                                 " failed: " + reason + " (is another pusher already running?)");
    }
    port_ = port;
}

void XsensFullBodyPlugin::close_socket()
{
    if (socket_fd_ >= 0)
    {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool XsensFullBodyPlugin::update()
{
    const ssize_t received = ::recv(socket_fd_, buffer_.data(), buffer_.size(), 0);
    if (received <= 0)
    {
        return false; // timeout, or MVN is not streaming yet
    }

    const auto frame = deserialize_teleop_frame(buffer_.data(), static_cast<size_t>(received));
    if (!frame)
    {
        ++stats_.dropped_malformed;
        return false;
    }

    // A payload larger than the collection was created for would be rejected by push_buffer
    // anyway; catching it here names the real number instead of an OpenXR error code.
    if (frame->payload_size > pusher_.config().max_flatbuffer_size)
    {
        ++stats_.dropped_malformed;
        std::cerr << "[XsensFullBody] payload " << frame->payload_size << " B exceeds max_flatbuffer_size "
                  << pusher_.config().max_flatbuffer_size << " -- raise it on BOTH pusher and reader" << std::endl;
        return false;
    }

    if (!verify_full_body_payload(frame->payload, frame->payload_size))
    {
        ++stats_.dropped_unverified;
        return false;
    }

    // seq is per-emitter, monotonic and sent-only. It resets to 0 when MVN starts a new session,
    // so a step backwards is a session boundary rather than an error. Note a gap is NOT reliable
    // evidence of transport loss: MVN also skips seq for frames it declines to build.
    if (have_seq_)
    {
        if (frame->seq == 0 && expected_seq_ != 0)
        {
            ++stats_.session_resets;
            last_sample_time_ns_ = 0;
            std::cout << "[XsensFullBody] sequence reset -> new MVN session" << std::endl;
        }
        else if (frame->seq < expected_seq_)
        {
            ++stats_.dropped_stale;
            return false; // duplicate or reordered datagram
        }
        else if (frame->seq > expected_seq_)
        {
            ++stats_.sequence_gaps;
        }
    }
    have_seq_ = true;
    expected_seq_ = frame->seq + 1;

    // MVN solves at millisecond resolution, so a sub-millisecond sample time means the frame did
    // not come from MVN's solver and should not be trusted as a clock reference.
    if (frame->sample_time_ns % NS_PER_MS != 0)
    {
        ++stats_.dropped_malformed;
        return false;
    }

    // A sample time that moves BACKWARDS is a timeline discontinuity, not a stale frame, and it
    // must not be dropped: scrubbing or restarting a recording rewinds MVN's clock while `seq`
    // keeps climbing, so rejecting these silently throws away every frame of a looped playback
    // (measured: 1315 frames lost in one run before this was fixed). Duplicates and reordering
    // are already handled by the `seq` check above, which is the authority on frame identity.
    if (frame->sample_time_ns < last_sample_time_ns_)
    {
        ++stats_.timeline_rewinds;
        std::cout << "[XsensFullBody] sample time moved backwards -- timeline rewind (scrub or "
                     "recording restart)"
                  << std::endl;
    }
    last_sample_time_ns_ = frame->sample_time_ns;

    // The header's sample_time_ns is on MVN's SEND-HOST clock -- a different domain from ours,
    // so it must not be published as the local common clock. Stamp the common clock here, at
    // publish time, and forward MVN's device clock verbatim alongside it.
    pusher_.push_buffer(frame->payload, frame->payload_size, core::os_monotonic_now_ns(), frame->raw_device_time_ns);

    ++stats_.delivered;
    stats_.last_seq = frame->seq;
    stats_.last_size = frame->payload_size;
    stats_.last_hash = fnv1a64(frame->payload, frame->payload_size);
    return true;
}

} // namespace xsens_full_body
} // namespace plugins
