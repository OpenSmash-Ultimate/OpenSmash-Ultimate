/* H.264 for the video decode engine: the parameter sets written back from a job's picture setup,
 * and the decoder the frames go to (gpu_h264.h). gpu_host1x.c drives it. */
#define _CRT_SECURE_NO_WARNINGS
#include <string.h>

#include "gpu_h264.h"

/* ---- the parameter sets ----------------------------------------------------------------- */
typedef struct Bits { uint8_t *out; uint32_t cap, n, bit, zeros; int over; uint8_t acc; } Bits;   /* n bytes written, emulation prevention included */

static void put_byte(Bits *b, uint8_t v) {
    if (b->zeros >= 2 && v <= 3) {   /* (00 00 0x in the payload: 00 00 03 0x) */
        if (b->n >= b->cap) { b->over = 1; return; }
        b->out[b->n++] = 3;
        b->zeros = 0;
    }
    if (b->n >= b->cap) { b->over = 1; return; }
    b->out[b->n++] = v;
    b->zeros = v ? 0 : b->zeros + 1;
}
static void put_bits(Bits *b, uint32_t count, uint32_t v) {
    while (count--) {
        b->acc = (uint8_t)(b->acc << 1 | ((v >> count) & 1));
        if (++b->bit == 8) { put_byte(b, b->acc); b->bit = 0; b->acc = 0; }
    }
}
static void put_ue(Bits *b, uint32_t v) {
    uint64_t x = (uint64_t)v + 1;
    uint32_t len = 0;
    while ((x >> len) > 1) len++;
    put_bits(b, len, 0);
    put_bits(b, len + 1, (uint32_t)x);
}
static void put_se(Bits *b, int32_t v) { put_ue(b, v > 0 ? 2u * (uint32_t)v - 1 : 2u * (uint32_t)-v); }
static void start(Bits *b, uint8_t nal) {   /* a start code and the NAL header, outside emulation prevention */
    static const uint8_t code[4] = {0, 0, 0, 1};
    for (int i = 0; i < 4; i++) {
        if (b->n >= b->cap) { b->over = 1; return; }
        b->out[b->n++] = code[i];
    }
    if (b->n >= b->cap) { b->over = 1; return; }
    b->out[b->n++] = nal;
    b->zeros = 0;
    b->bit = 0;
    b->acc = 0;
}
static void trailing(Bits *b) {   /* rbsp_trailing_bits */
    put_bits(b, 1, 1);
    if (b->bit) put_bits(b, 8 - b->bit, 0);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

uint32_t h264_write_headers(const uint8_t *s, uint8_t *out, uint32_t cap, const char **why) {
    uint64_t f = (uint64_t)rd32(s + 0xB0) | (uint64_t)rd32(s + 0xB4) << 32;
    uint32_t poc_type = (uint32_t)(f >> 14) & 3, chroma = (uint32_t)(f >> 12) & 3;
    uint32_t width = rd32(s + 0x64), height = rd32(s + 0x68), frames_only = rd32(s + 0x60) & 1;
    uint32_t l0 = rd32(s + 0x78), l1 = rd32(s + 0x7C);
    const char *no = NULL;
    for (uint32_t i = 0x1C0; i < 0x2A0; i++)
        if (s[i] != 16) no = "scaling lists that are not flat";
    if (poc_type == 1) no = "pic_order_cnt_type 1";
    if (chroma != 1) no = "a chroma format other than 4:2:0";
    if (!width || !height || width > 512 || height > 512 || l0 > 31 || l1 > 31) no = "a picture setup out of range";
    if (why) *why = no;
    if (no) return 0;
    /* The reference frames the sequence keeps (max_num_ref_frames) are not in the setup; the PPS's
     * default count of list 0 is the encoder's (x264 writes its --ref there), and too many would
     * change the default reference lists of B pictures (8.2.4.2.3). */
    uint32_t refs = (l0 > l1 ? l0 : l1) + 1;
    uint32_t mbs = width * height * (frames_only ? 1 : 2);
    /* the smallest level that takes the frame size and whose decoded picture buffer holds them
     * (A.3.1, table A-1), so that a decoder holds back as few pictures as it can */
    static const struct { uint8_t level; uint32_t frame_mbs, dpb_mbs; } levels[] = {
        {30, 1620, 8100}, {31, 3600, 18000}, {32, 5120, 20480}, {40, 8192, 32768}, {42, 8704, 34816}, {50, 22080, 110400}, {51, 36864, 184320}};
    uint32_t level = 52;
    for (uint32_t i = 0; i < sizeof levels / sizeof *levels; i++)
        if (levels[i].frame_mbs >= mbs && levels[i].dpb_mbs / mbs >= refs) { level = levels[i].level; break; }
    Bits b = {out, cap, 0, 0, 0, 0, 0};
    start(&b, 0x67);   /* SPS */
    put_bits(&b, 8, 100);   /* profile_idc: High */
    put_bits(&b, 8, 0);     /* constraint flags */
    put_bits(&b, 8, level);
    put_ue(&b, 0);          /* seq_parameter_set_id */
    put_ue(&b, chroma);
    put_ue(&b, 0);          /* bit_depth_luma_minus8 */
    put_ue(&b, 0);          /* bit_depth_chroma_minus8 */
    put_bits(&b, 1, 0);     /* qpprime_y_zero_transform_bypass_flag */
    put_bits(&b, 1, 0);     /* seq_scaling_matrix_present_flag */
    put_ue(&b, (uint32_t)(f >> 8) & 15);   /* log2_max_frame_num_minus4 */
    put_ue(&b, poc_type);
    if (poc_type == 0) put_ue(&b, rd32(s + 0x58) & 15);   /* log2_max_pic_order_cnt_lsb_minus4 */
    put_ue(&b, refs);       /* max_num_ref_frames */
    put_bits(&b, 1, 0);     /* gaps_in_frame_num_value_allowed_flag */
    put_ue(&b, width - 1);
    put_ue(&b, height - 1);
    put_bits(&b, 1, frames_only);
    if (!frames_only) put_bits(&b, 1, (uint32_t)f & 1);   /* mb_adaptive_frame_field_flag */
    put_bits(&b, 1, (uint32_t)(f >> 1) & 1);   /* direct_8x8_inference_flag */
    put_bits(&b, 1, 0);     /* frame_cropping_flag */
    put_bits(&b, 1, 1);     /* vui_parameters_present_flag: only for the bitstream restriction */
    for (int i = 0; i < 8; i++) put_bits(&b, 1, 0);   /* aspect ratio, overscan, video signal, chroma location, timing, NAL HRD, VCL HRD, pic_struct */
    put_bits(&b, 1, 1);     /* bitstream_restriction_flag */
    put_bits(&b, 1, 1);     /* motion_vectors_over_pic_boundaries_flag */
    put_ue(&b, 2);          /* max_bytes_per_pic_denom */
    put_ue(&b, 1);          /* max_bits_per_mb_denom */
    put_ue(&b, 16);         /* log2_max_mv_length_horizontal */
    put_ue(&b, 16);         /* log2_max_mv_length_vertical */
    /* No picture held back for display order: the engine puts each picture in its surface as it
     * is decoded, and the game shows them in display order itself. Media Foundation's decoder
     * (low latency mode) then gives every picture at once, B pictures too, in decoding order
     * (all 184 of Sephiroth's movie, each bit for bit as FFmpeg decodes it). With 2 here it held
     * pictures back up to 7 jobs, and the movie's last 2 were never given before VIC asked. */
    put_ue(&b, 0);          /* max_num_reorder_frames */
    put_ue(&b, refs);       /* max_dec_frame_buffering */
    trailing(&b);
    start(&b, 0x68);   /* PPS */
    put_ue(&b, 0);          /* pic_parameter_set_id */
    put_ue(&b, 0);          /* seq_parameter_set_id */
    put_bits(&b, 1, rd32(s + 0x70) & 1);   /* entropy_coding_mode_flag */
    put_bits(&b, 1, rd32(s + 0x74) & 1);   /* bottom_field_pic_order_in_frame_present_flag */
    put_ue(&b, 0);          /* num_slice_groups_minus1 */
    put_ue(&b, l0);
    put_ue(&b, l1);
    put_bits(&b, 1, (uint32_t)(f >> 2) & 1);   /* weighted_pred_flag */
    put_bits(&b, 2, (uint32_t)(f >> 32) & 3);  /* weighted_bipred_idc */
    put_se(&b, sext((uint32_t)(f >> 16) & 63, 6));   /* pic_init_qp_minus26 */
    put_se(&b, 0);          /* pic_init_qs_minus26 */
    put_se(&b, sext((uint32_t)(f >> 22) & 31, 5));   /* chroma_qp_index_offset */
    put_bits(&b, 1, rd32(s + 0x80) & 1);   /* deblocking_filter_control_present_flag */
    put_bits(&b, 1, (uint32_t)(f >> 3) & 1);   /* constrained_intra_pred_flag */
    put_bits(&b, 1, rd32(s + 0x84) & 1);   /* redundant_pic_cnt_present_flag */
    put_bits(&b, 1, rd32(s + 0x88) & 1);   /* transform_8x8_mode_flag */
    put_bits(&b, 1, 0);     /* pic_scaling_matrix_present_flag */
    put_se(&b, sext((uint32_t)(f >> 27) & 31, 5));   /* second_chroma_qp_index_offset */
    trailing(&b);
    return b.over ? 0 : b.n;
}

/* The next start code at or after i: its position, and *len its length (3 or 4); size if none. */
static uint32_t next_unit(const uint8_t *d, uint32_t size, uint32_t i, uint32_t *len) {
    for (; i + 3 <= size; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (i > 0 && d[i - 1] == 0) { *len = 4; return i - 1; }
            *len = 3;
            return i;
        }
    *len = 0;
    return size;
}

int h264_starts_idr(const uint8_t *d, uint32_t size) {
    uint32_t len, i = 0;
    while ((i = next_unit(d, size, i, &len)) < size) {
        if (i + len >= size) return 0;
        uint32_t type = d[i + len] & 31;
        if (type >= 1 && type <= 5) return type == 5;
        i += len;
    }
    return 0;
}

uint32_t h264_strip_end_units(const uint8_t *d, uint32_t size) {
    uint32_t len, i = 0;
    while ((i = next_unit(d, size, i, &len)) < size) {
        if (i + len < size && ((d[i + len] & 31) == 10 || (d[i + len] & 31) == 11)) return i;
        i += len;
    }
    return size;
}

/* ---- the decoder: Media Foundation's ----------------------------------------------------- */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <stdio.h>

/* (defined here: mfplat.dll is loaded when a movie first needs it, and these come from no import library) */
DEFINE_GUID(H264_CLSID_DECODER, 0x62CE7E72, 0x4C71, 0x4D20, 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D);   /* CLSID_CMSH264DecoderMFT */
DEFINE_GUID(H264_IID_TRANSFORM, 0xBF94C121, 0x5B05, 0x4E6F, 0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D);   /* IID_IMFTransform */
DEFINE_GUID(H264_LOW_LATENCY, 0x9C27891A, 0xED7A, 0x40E1, 0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE);     /* CODECAPI_AVLowLatencyMode */

static struct {
    int state;   /* 0 not yet, 1 ready, -1 none */
    IMFTransform *t;
    HRESULT (WINAPI *create_type)(IMFMediaType **);
    HRESULT (WINAPI *create_sample)(IMFSample **);
    HRESULT (WINAPI *create_buffer)(DWORD, IMFMediaBuffer **);
    uint32_t width, height;   /* the output's (aligned) frame size */
} mf;

static int pick_output(void) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *type = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(mf.t, 0, i, &type))) return 0;
        GUID sub;
        int ok = SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12) &&
                 SUCCEEDED(IMFTransform_SetOutputType(mf.t, 0, type, 0));
        if (ok) {
            UINT64 size = 0;
            if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size))) size = 0;   /* (0: pictures are skipped) */
            mf.width = (uint32_t)(size >> 32);
            mf.height = (uint32_t)size;
        }
        IMFMediaType_Release(type);
        if (ok) return 1;
    }
}

static int mf_start(void) {
    if (mf.state) return mf.state > 0;
    mf.state = -1;
    HMODULE lib = LoadLibraryA("mfplat.dll");
    HRESULT (WINAPI *startup)(ULONG, DWORD) = lib ? (HRESULT (WINAPI *)(ULONG, DWORD))(void *)GetProcAddress(lib, "MFStartup") : NULL;
    mf.create_type = lib ? (HRESULT (WINAPI *)(IMFMediaType **))(void *)GetProcAddress(lib, "MFCreateMediaType") : NULL;
    mf.create_sample = lib ? (HRESULT (WINAPI *)(IMFSample **))(void *)GetProcAddress(lib, "MFCreateSample") : NULL;
    mf.create_buffer = lib ? (HRESULT (WINAPI *)(DWORD, IMFMediaBuffer **))(void *)GetProcAddress(lib, "MFCreateMemoryBuffer") : NULL;
    if (!startup || !mf.create_type || !mf.create_sample || !mf.create_buffer || FAILED(startup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
        fprintf(stderr, "[nvdec] H.264: Media Foundation is not there (a Windows N edition without its Media Feature Pack?): "
                        "H.264 movies stay black\n");
        return 0;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* (this thread's: already set up either way is fine) */
    if (FAILED(CoCreateInstance(&H264_CLSID_DECODER, NULL, CLSCTX_INPROC_SERVER, &H264_IID_TRANSFORM, (void **)&mf.t))) {
        fprintf(stderr, "[nvdec] H.264: Windows' H.264 decoder would not start: H.264 movies stay black\n");
        return 0;
    }
    IMFAttributes *attrs = NULL;
    if (SUCCEEDED(IMFTransform_GetAttributes(mf.t, &attrs)) && attrs) {
        IMFAttributes_SetUINT32(attrs, &H264_LOW_LATENCY, TRUE);   /* (no holding back beyond what the stream says) */
        IMFAttributes_Release(attrs);
    }
    IMFMediaType *in = NULL;
    int ok = SUCCEEDED(mf.create_type(&in)) && SUCCEEDED(IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)) &&
             SUCCEEDED(IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_H264)) && SUCCEEDED(IMFTransform_SetInputType(mf.t, 0, in, 0)) &&
             pick_output();
    if (in) IMFMediaType_Release(in);
    if (!ok) {
        fprintf(stderr, "[nvdec] H.264: Windows' H.264 decoder takes no NV12 output: H.264 movies stay black\n");
        IMFTransform_Release(mf.t);
        mf.t = NULL;
        return 0;
    }
    IMFTransform_ProcessMessage(mf.t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(mf.t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    fprintf(stderr, "[nvdec] H.264: decoded by Windows' H.264 decoder (Media Foundation)\n");
    mf.state = 1;
    return 1;
}

/* Every picture the decoder has ready, to `got`. */
static void take_pictures(H264Picture got, void *user) {
    for (int changes = 0;;) {
        MFT_OUTPUT_STREAM_INFO info;
        memset(&info, 0, sizeof info);
        if (FAILED(IMFTransform_GetOutputStreamInfo(mf.t, 0, &info))) return;
        IMFSample *sample = NULL;
        IMFMediaBuffer *buf = NULL;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            if (FAILED(mf.create_sample(&sample))) return;
            if (FAILED(mf.create_buffer(info.cbSize ? info.cbSize : 1920 * 1088 * 3 / 2, &buf))) { IMFSample_Release(sample); return; }
            IMFSample_AddBuffer(sample, buf);
            IMFMediaBuffer_Release(buf);
            buf = NULL;
        }
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        ob.pSample = sample;
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(mf.t, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {   /* the first picture's size: the output type again */
            if (ob.pSample && ob.pSample != sample) IMFSample_Release(ob.pSample);
            if (sample) IMFSample_Release(sample);
            if (++changes > 3 || !pick_output()) return;
            continue;
        }
        if (FAILED(hr)) {   /* (MF_E_TRANSFORM_NEED_MORE_INPUT: nothing ready) */
            if (ob.pSample && ob.pSample != sample) IMFSample_Release(ob.pSample);
            if (sample) IMFSample_Release(sample);
            return;
        }
        IMFSample *out = ob.pSample;
        if (!out) {   /* (no picture after all) */
            if (sample) IMFSample_Release(sample);
            return;
        }
        LONGLONG tag = 0;
        IMFSample_GetSampleTime(out, &tag);
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(out, &buf))) {
            /* Locked whole, a frame is contiguous (a 2D buffer gives a packed copy): its rows' pitch is
             * what the length says, the chroma rows after the luma ones. */
            BYTE *data = NULL;
            DWORD length = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &data, NULL, &length))) {
                uint32_t pitch = mf.height ? (uint32_t)((uint64_t)length * 2 / (3ull * mf.height)) : 0;
                if (data && mf.width && pitch >= mf.width)
                    got(user, (uint64_t)tag, mf.width, mf.height, data, data + (size_t)pitch * mf.height, pitch);
                IMFMediaBuffer_Unlock(buf);
            }
            IMFMediaBuffer_Release(buf);
        }
        IMFSample_Release(out);
        if (sample && sample != out) IMFSample_Release(sample);
    }
}

int h264_decode(const uint8_t *data, uint32_t size, uint64_t tag, H264Picture got, void *user) {
    if (!mf_start()) return -1;
    IMFSample *sample = NULL;
    IMFMediaBuffer *buf = NULL;
    BYTE *p = NULL;
    if (FAILED(mf.create_sample(&sample))) return 0;
    if (FAILED(mf.create_buffer(size, &buf)) || FAILED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        if (buf) IMFMediaBuffer_Release(buf);
        IMFSample_Release(sample);
        return 0;
    }
    memcpy(p, data, size);
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_SetCurrentLength(buf, size);
    IMFSample_AddBuffer(sample, buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_SetSampleTime(sample, (LONGLONG)tag);
    IMFSample_SetSampleDuration(sample, 1);
    HRESULT hr = IMFTransform_ProcessInput(mf.t, 0, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {   /* pictures waiting to be taken first */
        take_pictures(got, user);
        hr = IMFTransform_ProcessInput(mf.t, 0, sample, 0);
    }
    IMFSample_Release(sample);
    take_pictures(got, user);
    return SUCCEEDED(hr);
}

void h264_drain(H264Picture got, void *user) {
    if (mf.state <= 0) return;
    IMFTransform_ProcessMessage(mf.t, MFT_MESSAGE_COMMAND_DRAIN, 0);
    take_pictures(got, user);
    IMFTransform_ProcessMessage(mf.t, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(mf.t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}
#else
#include <stdio.h>
int h264_decode(const uint8_t *data, uint32_t size, uint64_t tag, H264Picture got, void *user) {
    (void)data; (void)size; (void)tag; (void)got; (void)user;
    fprintf(stderr, "[nvdec] H.264: no decoder in this build (Windows only so far): H.264 movies stay black\n");
    return -1;
}
void h264_drain(H264Picture got, void *user) { (void)got; (void)user; }
#endif
