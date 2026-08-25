# xsens_full_body plugin

Receives Xsens MVN Studio's **Isaac Teleop** UDP stream and republishes it as an Isaac Teleop
tensor collection, readable through the `body.xsens` vendor.

```
MVN Studio  --UDP 9764 (XTLP)-->  xsens_full_body_plugin  -->  collection "xsens_full_body"
                                                                tensor "full_body_pose"
```

## Why it is this small

MVN Studio does the skeleton conversion itself: its network-streamer preset converts the
23-segment MVN skeleton to the vendor-neutral 24-joint `XR_BD_body_tracking` layout and emits a
`core::FullBodyPose` FlatBuffer — the same schema this repository defines. So the plugin needs no
vendor SDK, and **forwards the payload bytes verbatim**; re-serializing would risk silently
changing a pose and buys nothing.

## Running

```bash
source ~/.cloudxr/run/cloudxr.env          # in this shell and every consumer's
./xsens_full_body_plugin [collection_id] [udp_port] [max_flatbuffer_size]
#  defaults:              xsens_full_body   9764       4096
```

Then in MVN Studio: **Options → Network Streamer → preset "Isaac Teleop"**, tick the destination
row, and move in the suit or press **Play** on a recording.

Reader side:

```python
tracker = deviceio.FullBodyTracker()
vendor  = deviceio.VendorConfig([(tracker, deviceio.TrackerVendor("body.xsens", {
    "collection_id": "xsens_full_body", "max_flatbuffer_size": "4096"}))])
# NB: pass `vendor` to get_required_extensions() as well, or the pushed-tensor extensions are
# never requested and the session silently never sees the collection.
```

## Things that will bite

- **Order matters.** The pusher creates the collection, so a consumer started first reports "no
  collection found" forever.
- **A `collection_id` mismatch fails silently and forever** — the two sides rendezvous on that
  string. A `max_flatbuffer_size` mismatch, by contrast, throws loudly and names the right value.
  So when a reader sees nothing, suspect the collection id first.
- **22 of 24 joints valid is correct.** MVN has no finger tracking, so `LEFT_HAND` (22) and
  `RIGHT_HAND` (23) carry a copy of the wrist pose flagged invalid, and `all_joint_poses_tracked`
  is structurally always false. Consult the per-joint flags.
- **A sequence gap is not proof of packet loss.** `seq` is sent-only: MVN also skips it for frames
  it declines to build (invalid pose, fewer than 23 segments, any non-finite component). A step
  back to 0 is a new MVN session, not an error.
- **Timestamps are two different clocks.** The header's `sample_time_ns` is on MVN's send-host
  clock and is deliberately *not* published as the local common clock; the plugin stamps that
  itself at publish time and forwards MVN's device clock verbatim alongside it.

## Wire format

One frame per datagram: a 36-byte little-endian header, then the payload. See `teleop_wire.hpp`;
the authority is `bsn_stack/mvn/mvn_studio/src/picofullbody_core/teleop_wire.h`.

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `"XTLP"` |
| 4 | 2 | version = 1 |
| 6 | 2 | reserved — v1 receivers MUST ignore |
| 8 | 8 | `seq` (uint64) |
| 16 | 8 | `sample_time_ns` (int64, MVN send-host clock) |
| 24 | 8 | `raw_device_time_ns` (int64, MVN device clock) |
| 32 | 4 | `payload_len` (uint32) |

A live MVN pose is 820 bytes: 36 B header + 784 B payload (16 B table/vtable + 24 × 32 B joint
structs). Meters, **Y-up** (OpenXR), quaternions **xyzw**, absolute global poses.
