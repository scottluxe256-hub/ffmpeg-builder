/*
 * fdk-aac — CLI encoder ala ffmpeg.
 *
 * Input: APAPUN yang bisa di-demux/decode oleh libavformat/libavcodec
 * (audio: wav/mp3/flac/ogg/... ; video: mp4/mkv/mov/... -> audio track-nya
 * otomatis diambil, video track-nya diabaikan). Ini BUKAN "manggil ffmpeg
 * lagi" (tidak ada exec ke binary ffmpeg terpisah) — libavformat/libavcodec
 * di-link statis LANGSUNG ke dalam binary fdk-aac ini, cuma dipakai sebagai
 * demuxer+decoder. Yang encode ke AAC tetap murni libfdk-aac.
 *
 * Contoh:
 *   fdk-aac -i input.mp4 -profile:a aac_he -b:a 64k -ar 44100 output.m4a
 *   fdk-aac -i lagu.mp3 output.aac                      (semua default)
 *
 * Default kalau flag tidak diisi:
 *   -profile:a  aac_low   (AAC-LC)
 *   -b:a        128k
 *   -ar         ikut sample rate stream audio sumbernya
 *                (mau selalu dipaksa 44100? lihat komentar di main())
 *
 * Channel: kalau sumber punya >2 channel (mis. 5.1 di file video), otomatis
 * di-downmix ke stereo oleh libswresample. Mono tetap mono.
 *
 * Output:
 *   *.aac        -> raw ADTS stream
 *   *.m4a/*.mp4  -> di-mux ke MP4 pakai minimp4.h (single header, MIT)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <fdk-aac/aacenc_lib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

/* ---------------------------------------------------------------------- */
/* Antrean PCM sederhana (buffer dinamis) supaya decode & encode bisa      */
/* di-streaming tanpa harus muat seluruh file ke memory sekaligus.         */
/* ---------------------------------------------------------------------- */

typedef struct {
    int16_t *data;
    size_t   len;       /* jumlah int16 (sudah termasuk semua channel) */
    size_t   cap;
    int      channels;
} PcmQueue;

static void pq_init(PcmQueue *q, int channels) {
    q->data = NULL; q->len = 0; q->cap = 0; q->channels = channels;
}
static void pq_push(PcmQueue *q, const int16_t *samples, size_t n) {
    if (q->len + n > q->cap) {
        q->cap = (q->len + n) * 2 + 4096;
        q->data = realloc(q->data, q->cap * sizeof(int16_t));
    }
    memcpy(q->data + q->len, samples, n * sizeof(int16_t));
    q->len += n;
}
static void pq_pop_frames(PcmQueue *q, size_t frames) {
    size_t n = frames * q->channels;
    if (n >= q->len) { q->len = 0; return; }
    memmove(q->data, q->data + n, (q->len - n) * sizeof(int16_t));
    q->len -= n;
}
static void pq_free(PcmQueue *q) { free(q->data); q->data = NULL; }

/* ---------------------------------------------------------------------- */
/* Profile -> AOT (Audio Object Type) fdk-aac                              */
/* ---------------------------------------------------------------------- */

typedef enum { PROFILE_LC, PROFILE_HE, PROFILE_HE_V2, PROFILE_LD, PROFILE_ELD } Profile;

static int parse_profile(const char *s, Profile *out) {
    if (!strcmp(s, "aac_low"))   { *out = PROFILE_LC;    return 0; }
    if (!strcmp(s, "aac_he"))    { *out = PROFILE_HE;    return 0; }
    if (!strcmp(s, "aac_he_v2")) { *out = PROFILE_HE_V2; return 0; }
    if (!strcmp(s, "aac_ld"))    { *out = PROFILE_LD;    return 0; }
    if (!strcmp(s, "aac_eld"))   { *out = PROFILE_ELD;   return 0; }
    return -1;
}

/* Nilai AOT dari FDK_audio.h (AUDIO_OBJECT_TYPE) */
static int profile_to_aot(Profile p) {
    switch (p) {
        case PROFILE_LC:    return 2;  /* AOT_AAC_LC     */
        case PROFILE_HE:    return 5;  /* AOT_SBR        */
        case PROFILE_HE_V2: return 29; /* AOT_PS         */
        case PROFILE_LD:    return 23; /* AOT_ER_AAC_LD  */
        case PROFILE_ELD:   return 39; /* AOT_ER_AAC_ELD */
    }
    return 2;
}

static long parse_bitrate(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (*end == 'k' || *end == 'K') v *= 1000.0;
    return (long)v;
}

static int has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

/* ---------------------------------------------------------------------- */
/* minimp4 write callback                                                  */
/* ---------------------------------------------------------------------- */

static int mp4_write_cb(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *)token;
    fseek(f, (long)offset, SEEK_SET);
    return fwrite(buffer, 1, size, f) != size;
}

/* ---------------------------------------------------------------------- */
/* Encode satu frame PCM (frame_len sample/channel) & tulis ke output      */
/* ---------------------------------------------------------------------- */

static int encode_and_write(HANDLE_AACENCODER enc, int channels,
                             const int16_t *pcm, int frame_samples_per_ch,
                             int is_last, int is_m4a, MP4E_mux_t *mux, int mp4_track,
                             FILE *out_f, uint8_t *out_buf, size_t out_buf_size, int frame_len) {
    AACENC_BufDesc in_buf_desc = {0}, out_buf_desc = {0};
    AACENC_InArgs  in_args  = {0};
    AACENC_OutArgs out_args = {0};

    void *in_ptrs[1]; int in_sizes[1]; int in_ident[1] = { IN_AUDIO_DATA }; int in_el_size[1] = { sizeof(int16_t) };
    int in_samples = frame_samples_per_ch * channels;
    in_ptrs[0] = (void *)pcm;
    in_sizes[0] = (int)(in_samples * sizeof(int16_t));

    if (frame_samples_per_ch > 0) {
        in_buf_desc.numBufs = 1;
        in_buf_desc.bufs = in_ptrs;
        in_buf_desc.bufferIdentifiers = in_ident;
        in_buf_desc.bufSizes = in_sizes;
        in_buf_desc.bufElSizes = in_el_size;
        in_args.numInSamples = in_samples;
    } else {
        in_args.numInSamples = -1; /* flush */
    }

    void *out_ptrs[1] = { out_buf };
    int   out_sizes[1] = { (int)out_buf_size };
    int   out_ident[1] = { OUT_BITSTREAM_DATA };
    int   out_el_size[1] = { 1 };
    out_buf_desc.numBufs = 1;
    out_buf_desc.bufs = out_ptrs;
    out_buf_desc.bufferIdentifiers = out_ident;
    out_buf_desc.bufSizes = out_sizes;
    out_buf_desc.bufElSizes = out_el_size;

    AACENC_ERROR err = aacEncEncode(enc,
                                     frame_samples_per_ch > 0 ? &in_buf_desc : NULL,
                                     &out_buf_desc, &in_args, &out_args, NULL);
    if (err != AACENC_OK) return err == AACENC_ENCODE_EOF ? 0 : -1;

    if (out_args.numOutBytes > 0) {
        if (is_m4a) {
            MP4E_put_sample(mux, mp4_track, out_buf, out_args.numOutBytes, frame_len, MP4E_SAMPLE_DEFAULT);
        } else {
            fwrite(out_buf, 1, out_args.numOutBytes, out_f);
        }
    }
    (void)is_last;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* main                                                                     */
/* ---------------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Pemakaian:\n"
        "  %s -i input(apa saja: audio/video) [-profile:a aac_low|aac_he|aac_he_v2|aac_ld|aac_eld]\n"
        "     [-b:a 128k] [-ar 44100] output.m4a|output.aac\n"
        "\n"
        "Default: -profile:a aac_low  -b:a 128k  -ar (ikut sumber)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = NULL;
    Profile profile = PROFILE_LC;
    long bitrate = 128000;
    int target_rate = 0;
    int ar_set = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            input_path = argv[++i];
        } else if (!strcmp(argv[i], "-profile:a") && i + 1 < argc) {
            if (parse_profile(argv[++i], &profile) != 0) {
                fprintf(stderr, "fdk-aac: profile tidak dikenal '%s'\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "-b:a") && i + 1 < argc) {
            bitrate = parse_bitrate(argv[++i]);
        } else if (!strcmp(argv[i], "-ar") && i + 1 < argc) {
            target_rate = atoi(argv[++i]);
            ar_set = 1;
        } else if (argv[i][0] != '-') {
            output_path = argv[i];
        } else {
            fprintf(stderr, "fdk-aac: opsi tidak dikenal '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (!input_path || !output_path) { usage(argv[0]); return 1; }

    /* ---------------- buka & cari stream audio (bisa dari file video) --- */
    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, input_path, NULL, NULL) < 0) {
        fprintf(stderr, "fdk-aac: gagal membuka/membaca '%s' (format tidak dikenali?)\n", input_path);
        return 1;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "fdk-aac: gagal membaca info stream '%s'\n", input_path);
        return 1;
    }

    const AVCodec *decoder = NULL;
    int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audio_idx < 0) {
        fprintf(stderr, "fdk-aac: tidak ada audio track di '%s'\n", input_path);
        return 1;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_idx]->codecpar);
    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
        fprintf(stderr, "fdk-aac: gagal membuka decoder audio untuk '%s'\n", input_path);
        return 1;
    }

    int src_channels = dec_ctx->ch_layout.nb_channels;
    int target_channels = src_channels > 2 ? 2 : (src_channels < 1 ? 1 : src_channels);
    if (!ar_set) target_rate = dec_ctx->sample_rate > 0 ? dec_ctx->sample_rate : 44100;
    /* Mau default -ar SELALU 44100 walau sumbernya beda? ganti baris di atas
       jadi: if (!ar_set) target_rate = 44100; */

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, target_channels);

    SwrContext *swr = NULL;
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, target_rate,
                             &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                             0, NULL) < 0 || swr_init(swr) < 0) {
        fprintf(stderr, "fdk-aac: gagal inisialisasi resampler\n");
        return 1;
    }

    /* ---------------- buka encoder fdk-aac ------------------------------ */
    int aot = profile_to_aot(profile);
    int is_m4a = has_suffix(output_path, ".m4a") || has_suffix(output_path, ".mp4");

    HANDLE_AACENCODER enc;
    if (aacEncOpen(&enc, 0, target_channels) != AACENC_OK) {
        fprintf(stderr, "fdk-aac: gagal buka encoder\n");
        return 1;
    }
    aacEncoder_SetParam(enc, AACENC_AOT, aot);
    aacEncoder_SetParam(enc, AACENC_SAMPLERATE, target_rate);
    aacEncoder_SetParam(enc, AACENC_CHANNELMODE, target_channels == 1 ? MODE_1 : MODE_2);
    aacEncoder_SetParam(enc, AACENC_CHANNELORDER, 1);
    aacEncoder_SetParam(enc, AACENC_BITRATE, (UINT)bitrate);
    aacEncoder_SetParam(enc, AACENC_BITRATEMODE, 0); /* Constant */
    aacEncoder_SetParam(enc, AACENC_AFTERBURNER, 1);
    aacEncoder_SetParam(enc, AACENC_TRANSMUX, is_m4a ? TT_MP4_RAW : TT_MP4_ADTS);
    if (aacEncEncode(enc, NULL, NULL, NULL, NULL, NULL) != AACENC_OK) {
        fprintf(stderr, "fdk-aac: gagal inisialisasi parameter encoder\n");
        return 1;
    }
    AACENC_InfoStruct info = {0};
    aacEncInfo(enc, &info);
    int frame_len = info.frameLength;

    FILE *out_f = fopen(output_path, "wb");
    if (!out_f) { fprintf(stderr, "fdk-aac: tidak bisa membuat output '%s'\n", output_path); return 1; }

    MP4E_mux_t *mux = NULL;
    int mp4_track = -1;
    if (is_m4a) {
        mux = MP4E_open(0, 0, out_f, mp4_write_cb);
        mp4e_track_t tr = {0};
        tr.track_media_kind = e_audio;
        tr.language[0] = 'u'; tr.language[1] = 'n'; tr.language[2] = 'd';
        tr.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
        tr.time_scale = target_rate;
        tr.u.a.channelcount = target_channels;
        mp4_track = MP4E_add_track(mux, &tr);
        MP4E_set_dsi(mux, mp4_track, info.confBuf, info.confSize);
    }

    /* ---------------- decode loop (streaming) ---------------------------- */
    PcmQueue q; pq_init(&q, target_channels);
    uint8_t enc_out_buf[20480];

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    uint8_t  *swr_out = NULL;
    int       swr_out_linesize = 0;
    int       swr_out_cap = 0;

    #define DRAIN_QUEUE() \
        while (q.len / q.channels >= (size_t)frame_len) { \
            encode_and_write(enc, target_channels, q.data, frame_len, 0, is_m4a, mux, mp4_track, \
                              out_f, enc_out_buf, sizeof(enc_out_buf), frame_len); \
            pq_pop_frames(&q, frame_len); \
        }

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    int needed = av_rescale_rnd(swr_get_delay(swr, dec_ctx->sample_rate) + frame->nb_samples,
                                                 target_rate, dec_ctx->sample_rate, AV_ROUND_UP);
                    if (needed > swr_out_cap) {
                        if (swr_out) av_freep(&swr_out);
                        av_samples_alloc(&swr_out, &swr_out_linesize, target_channels, needed, AV_SAMPLE_FMT_S16, 0);
                        swr_out_cap = needed;
                    }
                    int converted = swr_convert(swr, &swr_out, needed,
                                                 (const uint8_t **)frame->data, frame->nb_samples);
                    if (converted > 0) pq_push(&q, (int16_t *)swr_out, (size_t)converted * target_channels);
                    DRAIN_QUEUE();
                }
            }
        }
        av_packet_unref(pkt);
    }
    /* flush decoder */
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
        int needed = av_rescale_rnd(swr_get_delay(swr, dec_ctx->sample_rate) + frame->nb_samples,
                                     target_rate, dec_ctx->sample_rate, AV_ROUND_UP);
        if (needed > swr_out_cap) {
            if (swr_out) av_freep(&swr_out);
            av_samples_alloc(&swr_out, &swr_out_linesize, target_channels, needed, AV_SAMPLE_FMT_S16, 0);
            swr_out_cap = needed;
        }
        int converted = swr_convert(swr, &swr_out, needed, (const uint8_t **)frame->data, frame->nb_samples);
        if (converted > 0) pq_push(&q, (int16_t *)swr_out, (size_t)converted * target_channels);
        DRAIN_QUEUE();
    }
    /* flush resampler (sample yang masih tertahan di internal buffer swr) */
    while (1) {
        int converted = swr_convert(swr, &swr_out, swr_out_cap, NULL, 0);
        if (converted <= 0) break;
        pq_push(&q, (int16_t *)swr_out, (size_t)converted * target_channels);
    }
    DRAIN_QUEUE();
    /* sisa terakhir yang kurang dari satu frame penuh, encode apa adanya */
    if (q.len / q.channels > 0) {
        encode_and_write(enc, target_channels, q.data, (int)(q.len / q.channels), 1,
                          is_m4a, mux, mp4_track, out_f, enc_out_buf, sizeof(enc_out_buf), frame_len);
    }
    /* flush encoder fdk-aac (bitstream yang masih tertahan) */
    for (;;) {
        int r = encode_and_write(enc, target_channels, NULL, 0, 1, is_m4a, mux, mp4_track,
                                  out_f, enc_out_buf, sizeof(enc_out_buf), frame_len);
        if (r != 0) break;
    }

    if (swr_out) av_freep(&swr_out);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    pq_free(&q);

    if (is_m4a) MP4E_close(mux);
    fclose(out_f);
    aacEncClose(&enc);

    fprintf(stderr, "fdk-aac: selesai -> %s (profile=%d, %ldbps, %dHz, %dch)\n",
            output_path, aot, bitrate, target_rate, target_channels);
    return 0;
}
