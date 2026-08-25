// SPDX-FileCopyrightText: Copyright (c) 2026 Xsens Technologies B.V. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <pusherio/schema_pusher.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace core
{
class OpenXRSession;
}

namespace plugins
{
namespace xsens_full_body
{

//! Counters reported on the periodic stats line; also the acceptance signal for a live run.
struct XsensFullBodyStats
{
    uint64_t delivered = 0;
    uint64_t dropped_malformed = 0;
    uint64_t dropped_unverified = 0;
    uint64_t dropped_stale = 0;
    uint64_t timeline_rewinds = 0;
    uint64_t sequence_gaps = 0;
    uint64_t session_resets = 0;
    uint64_t last_seq = 0;
    uint64_t last_hash = 0;
    size_t last_size = 0;
};

/*!
 * @brief Receives Xsens MVN's Isaac Teleop UDP stream and republishes it as a tensor collection.
 *
 * MVN Studio already converts its 23-segment skeleton to the vendor-neutral 24-joint
 * XR_BD_body_tracking layout (see the "Isaac Teleop" network-streamer preset), so the payload
 * arriving here is a `core::FullBodyPose` FlatBuffer -- byte-identical to what any other
 * full-body vendor produces. This plugin therefore **forwards the payload bytes verbatim** and
 * never re-serializes: re-packing would risk silently changing a pose, and buys nothing.
 *
 * Pairs with the `body.xsens` vendor on the reader side:
 *
 *     deviceio.VendorConfig([(tracker, deviceio.TrackerVendor("body.xsens", {
 *         "collection_id": "xsens_full_body", "max_flatbuffer_size": "4096"}))])
 */
class XsensFullBodyPlugin
{
public:
    XsensFullBodyPlugin(const std::string& collection_id, uint16_t udp_port, size_t max_flatbuffer_size);
    ~XsensFullBodyPlugin();

    XsensFullBodyPlugin(const XsensFullBodyPlugin&) = delete;
    XsensFullBodyPlugin& operator=(const XsensFullBodyPlugin&) = delete;

    /*!
     * @brief Block for one datagram (up to the socket timeout) and push it if it is valid.
     * @return true if a frame was delivered, false on timeout or a rejected datagram.
     */
    bool update();

    const XsensFullBodyStats& stats() const
    {
        return stats_;
    }

private:
    void open_socket(uint16_t port);
    void close_socket();

    int socket_fd_ = -1;
    uint16_t port_ = 0;
    bool have_seq_ = false;
    uint64_t expected_seq_ = 0;
    int64_t last_sample_time_ns_ = 0;
    std::vector<uint8_t> buffer_;
    XsensFullBodyStats stats_;

    std::shared_ptr<core::OpenXRSession> session_;
    core::SchemaPusher pusher_;
};

} // namespace xsens_full_body
} // namespace plugins
