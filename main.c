#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Library FDK-AAC API
#include <fdk-aac/aacenc_lib.h>

// Embedded Demuxer & Decoder API
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

int main(int argc, char *argv[]) {
    char *input_file = NULL;
    char *output_file = NULL;
    int bitrate = 64000;       // Default 64k
    int aot = 29;              // Default HE-AAC v2 (AOT 29)

    // Parse Argumen CLI
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input_file = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_file = argv[++i];
        else if (strcmp(argv[i], "-b:a") == 0 && i + 1 < argc) {
            bitrate = atoi(argv[++i]) * 1000;
        } 
        else if (strcmp(argv[i], "-profile:a") == 0 && i + 1 < argc) {
            char *profile = argv[++i];
            if (strcmp(profile, "aac_low") == 0) aot = 2;         // AAC-LC
            else if (strcmp(profile, "aac_he") == 0) aot = 5;      // HE-AAC v1
            else if (strcmp(profile, "aac_he_v2") == 0) aot = 29;  // HE-AAC v2
        }
    }

    if (!input_file || !output_file) {
        printf("=========================================================\n");
        printf(" Standalone Direct FDK-AAC Engine v1.0 (CLI)\n");
        printf("=========================================================\n");
        printf("Penggunaan: fdk-aac -i <input.mp4/mkv/wav> -profile:a <profile> -b:a <bitrate> -o <output.aac>\n\n");
        printf("Contoh: fdk-aac -i input.mp4 -profile:a aac_he_v2 -b:a 64k -o output.aac\n");
        return 1;
    }

    // 1. DEMUXING (Buka Container Input MP4/MKV)
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, input_file, NULL, NULL) < 0) {
        printf("Error: Gagal membongkar file container input '%s'!\n", input_file);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        printf("Error: Gagal membaca stream info dari input!\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    // Cari Stream Audio
    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
            break;
        }
    }

    if (audio_stream_idx == -1) {
        printf("Error: Tidak ditemukan stream audio di dalam file input!\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    // 2. SETUP INTERNAL DECODER
    AVCodecParameters *codecpar = fmt_ctx->streams[audio_stream_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext *dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, codecpar);

    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
        printf("Error: Gagal menginisialisasi decoder audio internal!\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int out_sample_rate = dec_ctx->sample_rate > 0 ? dec_ctx->sample_rate : 48000;
    int out_channels = 2; // Stereo

    // 3. SETUP ENCODER LIBFDK-AAC
    HANDLE_AACENCODER hAac;
    if (aacEncOpen(&hAac, 0, out_channels) != AACENC_OK) {
        printf("Error: Gagal inisialisasi FDK-AAC Encoder Engine!\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    aacEncoder_SetParam(hAac, AACENC_AOT, aot);
    aacEncoder_SetParam(hAac, AACENC_SAMPLERATE, out_sample_rate);
    aacEncoder_SetParam(hAac, AACENC_CHANNELMODE, MODE_2); // Stereo
    aacEncoder_SetParam(hAac, AACENC_BITRATE, bitrate);
    aacEncoder_SetParam(hAac, AACENC_TRANSMUX, 2);        // ADTS Header Stream

    // 4. SETUP RESAMPLER (Konversi Audio Internal ke S16 Stereo PCM)
    SwrContext *swr_ctx = swr_alloc();
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&swr_ctx, 
                        &out_ch_layout, AV_SAMPLE_FMT_S16, out_sample_rate,
                        &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                        0, NULL);
    swr_init(swr_ctx);

    FILE *out_file = fopen(output_file, "wb");
    if (!out_file) {
        printf("Error: Gagal membuka file output '%s'!\n", output_file);
        return 1;
    }

    printf("[Engine] Processing Direct Input: %s -> %s [Profile: %d, Bitrate: %d bps]\n", 
            input_file, output_file, aot, bitrate);

    // 5. PROCESSING LOOP
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    
    int16_t pcm_buffer[2048 * 2]; // 2048 samples stereo 16-bit
    uint8_t aac_buffer[2048];

    void *in_buffers[] = { pcm_buffer };
    int in_buffer_ids[] = { IN_AUDIO_DATA };
    int in_buffer_sizes[] = { sizeof(pcm_buffer) };
    int in_buffer_el_sizes[] = { sizeof(int16_t) };

    void *out_buffers[] = { aac_buffer };
    int out_buffer_ids[] = { OUT_BITSTREAM_DATA };
    int out_buffer_sizes[] = { sizeof(aac_buffer) };
    int out_buffer_el_sizes[] = { sizeof(uint8_t) };

    AACENC_BufDesc in_buf_desc = { 1, in_buffers, in_buffer_ids, in_buffer_sizes, in_buffer_el_sizes };
    AACENC_BufDesc out_buf_desc = { 1, out_buffers, out_buffer_ids, out_buffer_sizes, out_buffer_el_sizes };
    AACENC_InArgs in_args = { 0 };
    AACENC_OutArgs out_args = { 0 };

    uint8_t *swr_output_buf = (uint8_t *)pcm_buffer;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == audio_stream_idx) {
            if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    
                    int converted_samples = swr_convert(swr_ctx, 
                                                        &swr_output_buf, 2048,
                                                        (const uint8_t **)frame->data, frame->nb_samples);

                    if (converted_samples > 0) {
                        in_args.numInSamples = converted_samples * out_channels;
                        
                        if (aacEncEncode(hAac, &in_buf_desc, &out_buf_desc, &in_args, &out_args) == AACENC_OK) {
                            if (out_args.numOutBytes > 0) {
                                fwrite(aac_buffer, 1, out_args.numOutBytes, out_file);
                            }
                        }
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    printf("[Engine] Selesai! Audio AAC tersimpan di: %s\n", output_file);

    // Cleanup Resources
    fclose(out_file);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr_ctx);
    aacEncClose(&hAac);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}
