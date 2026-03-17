#include "encode_pcm_to_mp2.h"
#include "output_log.h"

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

// 检查编码器是否支持指定的采样格式。
// 例如这里希望输入 S16 格式 PCM，就先要确认编码器支持它。
static int check_sample_fmt(const AVCodec* pCodec, enum AVSampleFormat sample_fmt)
{
    const void* pConfigs = nullptr;
    int nb_configs = 0;
    const auto* p = static_cast<const enum AVSampleFormat*>(pConfigs);

    if (avcodec_get_supported_config(nullptr, pCodec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
        &pConfigs, &nb_configs) < 0)
    {
        return 0;
    }

    p = static_cast<const enum AVSampleFormat*>(pConfigs);

    for (int i = 0; p && i < nb_configs; i++)
    {
        if (p[i] == sample_fmt)
            return 1;
    }

    return 0;
}

// 从编码器支持的采样率里，挑一个最接近 44100Hz 的值。
static int select_sample_rate(const AVCodec* pCodec)
{
    const void* pConfigs = nullptr;
    int nb_configs = 0;
    const auto* p = static_cast<const int*>(pConfigs);
    int best_samplerate = 0;

    if (avcodec_get_supported_config(nullptr, pCodec, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
        &pConfigs, &nb_configs) < 0 || !pConfigs || nb_configs <= 0)
    {
        return 44100;
    }

    p = static_cast<const int*>(pConfigs);
    for (int i = 0; i < nb_configs; i++)
    {
        if (!best_samplerate || std::abs(44100 - p[i]) < std::abs(44100 - best_samplerate))
            best_samplerate = p[i];
    }

    return best_samplerate;
}

// 从编码器支持的声道布局里选择一个。
// 这里使用的是新版 FFmpeg 的 AVChannelLayout 接口。
static int select_channel_layout(const AVCodec* pCodec, AVChannelLayout* p_ch_layout)
{
    const void* pConfigs = nullptr;
    int nb_configs = 0;
    const auto* p = static_cast<const AVChannelLayout*>(pConfigs);
    int best_nb_channels = 0;
    int best_index = -1;

    if (avcodec_get_supported_config(nullptr, pCodec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
        &pConfigs, &nb_configs) < 0 || !pConfigs || nb_configs <= 0)
    {
        av_channel_layout_default(p_ch_layout, 2);
        return 0;
    }

    p = static_cast<const AVChannelLayout*>(pConfigs);
    for (int i = 0; i < nb_configs; i++)
    {
        int nb_channels = p[i].nb_channels;

        if (nb_channels > best_nb_channels)
        {
            best_index = i;
            best_nb_channels = nb_channels;
        }
    }

    if (best_index < 0)
    {
        av_channel_layout_default(p_ch_layout, 2);
        return 0;
    }

    return av_channel_layout_copy(p_ch_layout, &p[best_index]);
}

// 把一帧原始 PCM 数据送进编码器，再把得到的 MP2 压缩包写入文件。
static int encode_audio_packet(AVCodecContext* pCodecCtx, AVFrame* pFrame, AVPacket* pPacket, FILE* p_output_f)
{
    int ret = avcodec_send_frame(pCodecCtx, pFrame);
    if (ret < 0)
    {
        output_log(LOG_ERROR, "avcodec_send_frame error: %d", ret);
        return ret;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(pCodecCtx, pPacket);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
        {
            output_log(LOG_ERROR, "avcodec_receive_packet error: %d", ret);
            return ret;
        }

        fwrite(pPacket->data, 1, pPacket->size, p_output_f);
        av_packet_unref(pPacket);
    }

    return 0;
}

int encode_pcm_to_mp2(const char* output_file_path)
{
    // 音频编码和视频编码在思路上很像：
    // 先准备原始帧 AVFrame，再把它送给编码器得到压缩包 AVPacket。
    AVCodecContext* pCodecCtx = nullptr;
    const AVCodec* pCodec = nullptr;
    AVPacket* pPacket = nullptr;
    AVFrame* pFrame = nullptr;
    FILE* p_output_f = nullptr;
    int ret = 0;
    float t = 0.0f;
    float tincr = 0.0f;

    output_log(LOG_INFO, "encode_pcm_to_mp2 start: %s", output_file_path);

    // 找 MP2 编码器。
    pCodec = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!pCodec)
    {
        output_log(LOG_ERROR, "avcodec_find_encoder(AV_CODEC_ID_MP2) error");
        ret = -1;
        goto end;
    }

    // 分配编码器上下文、压缩包对象、原始音频帧对象。
    pCodecCtx = avcodec_alloc_context3(pCodec);
    pPacket = av_packet_alloc();
    pFrame = av_frame_alloc();
    if (!pCodecCtx || !pPacket || !pFrame)
    {
        output_log(LOG_ERROR, "alloc encode objects error");
        ret = -1;
        goto end;
    }

    // 设置编码参数。
    pCodecCtx->bit_rate = 64000;
    pCodecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(pCodec, pCodecCtx->sample_fmt))
    {
        output_log(LOG_ERROR, "check_sample_fmt error");
        ret = -1;
        goto end;
    }

    // 选择一个编码器支持的采样率和声道布局。
    pCodecCtx->sample_rate = select_sample_rate(pCodec);
    if (select_channel_layout(pCodec, &pCodecCtx->ch_layout) < 0)
    {
        output_log(LOG_ERROR, "select_channel_layout error");
        ret = -1;
        goto end;
    }

    // 打开编码器。
    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        output_log(LOG_ERROR, "avcodec_open2 error");
        ret = -1;
        goto end;
    }

    // frame_size 表示编码器希望每次输入多少个采样点。
    pFrame->nb_samples = pCodecCtx->frame_size;
    pFrame->format = pCodecCtx->sample_fmt;

    // 把编码器使用的声道布局拷贝给原始帧。
    if (av_channel_layout_copy(&pFrame->ch_layout, &pCodecCtx->ch_layout) < 0)
    {
        output_log(LOG_ERROR, "av_channel_layout_copy error");
        ret = -1;
        goto end;
    }

    // 为一帧 PCM 数据申请缓冲区。
    if (av_frame_get_buffer(pFrame, 0) < 0)
    {
        output_log(LOG_ERROR, "av_frame_get_buffer error");
        ret = -1;
        goto end;
    }

    // 打开输出文件，输出的是 MP2 裸流。
    if (fopen_s(&p_output_f, output_file_path, "wb") != 0 || !p_output_f)
    {
        output_log(LOG_ERROR, "fopen_s error");
        ret = -1;
        goto end;
    }

    // 生成 440Hz 正弦波。
    // 440Hz 就是音乐中的标准音 A。
    tincr = static_cast<float>(2 * M_PI * 440.0 / pCodecCtx->sample_rate);
    for (int i = 0; i < 200; i++)
    {
        // 确保当前帧缓冲区可以安全写入。
        if (av_frame_make_writable(pFrame) < 0)
        {
            output_log(LOG_ERROR, "av_frame_make_writable error");
            ret = -1;
            goto end;
        }

        // S16 表示每个采样点是 16 位整数。
        auto* samples = reinterpret_cast<uint16_t*>(pFrame->data[0]);
        const int channels = pCodecCtx->ch_layout.nb_channels;
        for (int j = 0; j < pCodecCtx->frame_size; j++)
        {
            // 先生成一个声道的采样值。
            samples[channels * j] = static_cast<uint16_t>(std::sin(t) * 10000);

            // 如果是双声道或更多声道，这里简单复制到其它声道。
            for (int k = 1; k < channels; k++)
            {
                samples[channels * j + k] = samples[channels * j];
            }
            t += tincr;
        }

        // 把这一帧 PCM 数据送给编码器。
        ret = encode_audio_packet(pCodecCtx, pFrame, pPacket, p_output_f);
        if (ret < 0)
        {
            ret = -1;
            goto end;
        }
    }

    // 刷新编码器，把内部剩余压缩包全部取出。
    ret = encode_audio_packet(pCodecCtx, nullptr, pPacket, p_output_f);
    if (ret < 0)
    {
        ret = -1;
        goto end;
    }

    output_log(LOG_INFO, "encode_pcm_to_mp2 done");

end:
    // 统一释放资源。
    if (p_output_f)
        fclose(p_output_f);
    if (pCodecCtx)
        avcodec_free_context(&pCodecCtx);
    if (pPacket)
        av_packet_free(&pPacket);
    if (pFrame)
        av_frame_free(&pFrame);
    return ret;
}
