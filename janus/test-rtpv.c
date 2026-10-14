// SPDX-License-Identifier: GPL-3.0-or-later
// Round-trip fragmented and single NAL units at RTP payload boundaries.
#include <assert.h>
#include <string.h>
#include <linux/videodev2.h>
#include "src/rtpv.h"

static u8 restored[20000];
static uz restored_size;
static bool hevc;
static uint packets;
static u8 timestamp[4];

static void receive(const us_rtp_s *rtp) {
    assert(rtp->used <= US_RTP_TOTAL_SIZE);
    const u8 *payload = rtp->datagram + US_RTP_HEADER_SIZE;
    uz size = rtp->used - US_RTP_HEADER_SIZE;
    const uint type = hevc ? (payload[0] >> 1) & 63 : payload[0] & 31;
    const uint fu_type = hevc ? 49 : 28;
    if (!packets) memcpy(timestamp, rtp->datagram + 4, 4);
    assert(memcmp(timestamp, rtp->datagram + 4, 4) == 0);
    assert(rtp->datagram[3] == packets++);
    assert(((rtp->datagram[1] & 0x80) != 0) == rtp->last_of_frame);
    if (type == fu_type) {
        const uint header_size = hevc ? 3 : 2;
        const u8 fu = payload[header_size - 1];
        assert(((fu & 0x80) != 0) == rtp->first_of_frame);
        assert(((fu & 0x40) != 0) == rtp->last_of_frame);
        if (fu & 0x80) {
            restored[restored_size++] = hevc ? (payload[0] & 0x81) | ((fu & 63) << 1)
                                           : (payload[0] & 0xe0) | (fu & 31);
            if (hevc) restored[restored_size++] = payload[1];
        }
        payload += header_size;
        size -= header_size;
    }
    assert(restored_size + size <= sizeof(restored));
    memcpy(restored + restored_size, payload, size);
    restored_size += size;
}

int main(void) {
    const uz sizes[] = {10, US_RTP_TOTAL_SIZE - US_RTP_HEADER_SIZE,
        US_RTP_TOTAL_SIZE - US_RTP_HEADER_SIZE + 1, 8192};
    for (int codec = 0; codec < 2; ++codec) {
        hevc = codec;
        for (uz i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            u8 data[10000];
            memset(data, 0x55, sizeof(data));
            data[0] = data[1] = data[2] = 0;
            data[3] = 1;
            data[4] = hevc ? (19 << 1) | 1 : 0x65;
            data[5] = hevc ? 0x2b : 0x55; // Preserve HEVC layer and temporal IDs.
            us_frame_s frame = {.data = data, .used = sizes[i] + 4,
                .format = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264};
            restored_size = packets = 0;
            us_rtpv_s *rtpv = us_rtpv_init(receive);
            us_rtpv_wrap(rtpv, &frame, false);
            assert(restored_size == sizes[i]);
            assert(memcmp(restored, data + 4, restored_size) == 0);
            us_rtpv_destroy(rtpv);
        }
    }
    return 0;
}
