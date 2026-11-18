/* H.264 for the video decode engine (NVDEC, codec 3): what a software decoder needs that the
 * engine's job does not hold, and a decoder to give it to.
 *
 * The engine is handed each frame's slices (Annex B, start codes included) and a picture setup
 * with the sequence's and picture's parameter set fields already parsed; there is no SPS or PPS
 * in the bitstream. h264_write_headers writes them back from the setup (H.264, 7.3.2.1 and
 * 7.3.2.2), so that the slices can go to any decoder as an ordinary stream.
 *
 * The setup's layout (offsets from its start, read off the jobs of Sephiroth's Final Smash movie,
 * a 1280x720 High profile stream; each field checked against what decoding needed):
 *   0x48 the bitstream's size; 0x58 log2_max_pic_order_cnt_lsb_minus4; 0x5C
 *   delta_pic_order_always_zero_flag; 0x60 frame_mbs_only_flag; 0x64 the width in macroblocks;
 *   0x68 the height in map units; 0x70 entropy_coding_mode_flag; 0x74
 *   bottom_field_pic_order_in_frame_present_flag; 0x78, 0x7C num_ref_idx_l0/l1_default_active_minus1;
 *   0x80 deblocking_filter_control_present_flag; 0x84 redundant_pic_cnt_present_flag; 0x88
 *   transform_8x8_mode_flag; 0xB0 a 64-bit word of flags (H264_F_*); 0x1C0 the scaling lists,
 *   six 4x4 then two 8x8 (all 16: flat). */
#pragma once
#include <stdint.h>

enum {
    H264_SETUP_STREAM_LEN = 0x48,
    H264_SETUP_SIZE = 0x2A0,   /* the bytes the writer reads */
};

/* The flags word at 0xB0: bit 0 mbaff, 1 direct_8x8_inference, 2 weighted_pred, 3
 * constrained_intra_pred, 4 the picture is a reference, 5 field picture, 6 bottom field, 7
 * second field, 8-11 log2_max_frame_num_minus4, 12-13 chroma_format_idc, 14-15
 * pic_order_cnt_type, 16-21 pic_init_qp_minus26 (signed), 22-26 chroma_qp_index_offset (signed),
 * 27-31 second_chroma_qp_index_offset (signed), 32-33 weighted_bipred_idc, 34-40 the picture's
 * surface index (its luma surface is method 0x10C + index, its chroma 0x11D + index), 41-45 its
 * co-located buffer index, 46-61 frame_num. */
static inline uint32_t h264_surface_index(uint64_t flags) { return (uint32_t)(flags >> 34) & 0x7F; }

/* The SPS and PPS (id 0 each, with start codes) for the picture setup `setup` (H264_SETUP_SIZE
 * bytes); returns their size in bytes, 0 if `cap` is too small or the setup asks for what is not
 * written (pic_order_cnt_type 1, scaling lists that are not flat: *why says which). */
uint32_t h264_write_headers(const uint8_t *setup, uint8_t *out, uint32_t cap, const char **why);

/* Whether the bitstream's first slice is an IDR picture's (NAL type 5): a new sequence, which
 * gets the headers again. */
int h264_starts_idr(const uint8_t *data, uint32_t size);

/* The bitstream without the end of sequence / end of stream units (NAL types 10 and 11) the
 * game puts after every frame: a decoder would take them as the end of the stream. Returns the
 * size kept (the units before the first such one). */
uint32_t h264_strip_end_units(const uint8_t *data, uint32_t size);

/* A software decoder (Windows: Media Foundation's H.264 decoder, which every Windows but the N
 * editions has; elsewhere none yet). Pictures come out each with the tag of the access unit it
 * was decoded from, as NV12 (`pitch` bytes a row, the chroma plane `uv`): with the SPS above
 * (no reordering), Media Foundation's as each unit is decoded. */
typedef void (*H264Picture)(void *user, uint64_t tag, uint32_t width, uint32_t height, const uint8_t *y, const uint8_t *uv,
                            uint32_t pitch);
/* One access unit (headers included when it starts a sequence). 1 decoded (pictures, if any,
 * went to `got`), 0 the decoder refused it, -1 there is no decoder. */
int h264_decode(const uint8_t *data, uint32_t size, uint64_t tag, H264Picture got, void *user);
/* The pictures the decoder still holds for reordering, then a fresh start (the movie ended or a
 * new one starts with an IDR picture). */
void h264_drain(H264Picture got, void *user);
