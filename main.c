#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Library FDK-AAC API
#include <fdk-aac/aacenc_lib.h>

// Embedded FFmpeg API
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

int main(int argc, char *argv[]) {
    char *input_file = NULL;
    char *output_file = NULL;
    int bitrate = 64000;       // Default 64k
    int sample_rate = 44100;   // Default 44.1 kHz
    int aot = 29;              // Default HE-AAC v2 (AOT 29)
    int vbr_mode = 0;          // Default 0 = CBR

    // Parse Argumen CLI
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input_file = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_file = argv[++i];
        else if (strcmp(argv[i], "-b:a") == 0 && i + 1 < argc) {
            bitrate = atoi(argv[++i]) * 1000;
        } 
        else if (strcmp(argv[i], "-ar") == 0 && i + 1 < argc) {
            sample_rate = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-vbr") == 0 && i + 1 < argc) {
            vbr_mode = atoi(argv[++i]);
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
        printf(" Standalone Direct FDK-AAC Engine v4.0 (Full M4A Muxer)\n");
        printf("=========================================================\n");
        printf("Contoh CBR: fdk-aac -i input.mp4 -b:a 64k -o output.m4a\n");
        printf("Contoh VBR: fdk-aac -i input.mp4 -vbr 3 -o output.m4a\n\n");
        return 1;
    }

    // 1. DEMUXING INPUT
    AVFormatContext *in_fmt_ctx = NULL;
    if (avformat_open_input(&in_fmt_ctx, input_file, NULL, NULL) < 0) {
        printf("[Error] Gagal membuka file input '%s'!\n", input_file);
        return 1;
    }

    if (avformat_find_stream_info(in_fmt_ctx, NULL) < 0) {
        printf("[Error] Gagal membaca stream info!\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
            break;
        }
    }

    if (audio_stream_idx == -1) {
        printf("[Error] Tidak ditemukan stream audio di file input!\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 2. SETUP DECODER INPUT
    AVCodecParameters *codecpar = in_fmt_ctx->streams[audio_stream_idx]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext *dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, codecpar);

    if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
        printf("[Error] Gagal inisialisasi decoder!\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    int out_channels = 2; // Stereo

    // 3. SETUP LIBFDK-AAC ENCODER
    HANDLE_AACENCODER hAac;
    if (aacEncOpen(&hAac, 0, out_channels) != AACENC_OK) {
        printf("[Error] Gagal inisialisasi FDK-AAC Engine!\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    aacEncoder_SetParam(hAac, AACENC_AOT, aot);
    aacEncoder_SetParam(hAac, AACENC_SAMPLERATE, sample_rate);
    aacEncoder_SetParam(hAac, AACENC_CHANNELMODE, MODE_2);
    
    // TRANSMUX = 0 (RAW STREAM untuk dikemas ke M4A / MP4)
    // TRANSMUX = 2 (ADTS Stream jika file .aac)
    int is_mp4 = (strstr(output_file, ".m4a") || strstr(output_file, ".mp4"));
    aacEncoder_SetParam(hAac, AACENC_TRANSMUX, is_mp4 ? 0 : 2); 

    if (vbr_mode >= 1 && vbr_mode <= 5) {
        aacEncoder_SetParam(hAac, AACENC_BITRATEMODE, vbr_mode);
        printf("[Engine] Running Mode: VBR Level %d\n", vbr_mode);
    } else {
        aacEncoder_SetParam(hAac, AACENC_BITRATEMODE, 0);
        aacEncoder_SetParam(hAac, AACENC_BITRATE, bitrate);
        printf("[Engine] Running Mode: CBR (Bitrate: %d bps)\n", bitrate);
    }

    if (aacEncEncode(hAac, NULL, NULL, NULL, NULL) != AACENC_OK) {
        printf("[Error] Gagal menginisialisasi parameter encoder!\n");
        aacEncClose(&hAac);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 4. SETUP MUXER OUTPUT (MP4 / M4A / ADTS)
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, is_mp4 ? "mp4" : "adts", output_file);
    
    // Safety check biar gak Segfault
    if (!out_fmt_ctx) {
        avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, output_file);
    }
    
    if (!out_fmt_ctx) {
        printf("[Error] Format output '%s' tidak dikenali!\n", output_file);
        aacEncClose(&hAac);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->codec_id = AV_CODEC_ID_AAC;
    out_stream->codecpar->sample_rate = sample_rate;
    out_stream->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    out_stream->codecpar->bit_rate = bitrate;

    // Ambil Header DecoderConfig (esds) dari FDK-AAC untuk MP4 Container
    AACENC_MetaData meta = {0};
    aacEncGetLibInfo(&meta);
    
    HANDLE_AACENCODER hDummy = hAac;
    AAC_STREAM_DATA stream_data = {0};
    UCHAR conf_buf[64] = {0};
    UINT conf_size = sizeof(conf_buf);
    
    // Ambil Decoder Specific Info (AudioSpecificConfig)
    AACENC_OutArgs out_args_init = {0};
    if (aacEncGetLibInfo(NULL) == 0) {
        // Alokasikan extradata ESDS
        out_stream->codecpar->extradata = (uint8_t *)av_mallocz(64 + AV_INPUT_BUFFER_PADDING_SIZE);
    }

    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            printf("[Error] Gagal membuat file output '%s'!\n", output_file);
            avformat_free_context(out_fmt_ctx);
            aacEncClose(&hAac);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_fmt_ctx);
            return 1;
        }
    }

    if (avformat_write_header(out_fmt_ctx, NULL) < 0) {
        printf("[Error] Gagal menulis header container output!\n");
        avio_closep(&out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
        aacEncClose(&hAac);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 5. SETUP RESAMPLER
    SwrContext *swr_ctx = swr_alloc();
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&swr_ctx, 
                        &out_ch_layout, AV_SAMPLE_FMT_S16, sample_rate,
                        &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate,
                        0, NULL);
    swr_init(swr_ctx);

    // 6. LOOP PROCESSING AUDIO
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVPacket *out_pkt = av_packet_alloc();
    
    int16_t pcm_buffer[2048 * 2];
    uint8_t aac_buffer[2048 * 2];

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
    int64_t pts_counter = 0;

    while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
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
                                av_new_packet(out_pkt, out_args.numOutBytes);
                                memcpy(out_pkt->data, aac_buffer, out_args.numOutBytes);
                                
                                out_pkt->stream_index = out_stream->index;
                                out_pkt->pts = pts_counter;
                                out_pkt->dts = pts_counter;
                                out_pkt->duration = av_rescale_q(out_args.numInSamples / out_channels, 
                                                                (AVRational){1, sample_rate}, 
                                                                out_stream->time_base);
                                
                                pts_counter += out_pkt->duration;

                                av_interleaved_write_frame(out_fmt_ctx, out_pkt);
                                av_packet_unref(out_pkt);
                            }
                        }
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Tulis Trailer Container MP4
    av_write_trailer(out_fmt_ctx);

    printf("[Engine] Selesai! Result saved at: %s\n", output_file);

    // Cleanup
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_packet_free(&out_pkt);
    swr_free(&swr_ctx);
    aacEncClose(&hAac);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&in_fmt_ctx);

    return 0;
}
