#include "encode_yuv_to_h264.h"
#include "output_log.h"

#include <cstdio>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// 把一帧原始视频图像送进编码器，再把编码器吐出的压缩包写进文件。
// 解码时是 packet -> frame
// 编码时正好相反，是 frame -> packet
static int encode_video_packet(AVCodecContext* pCodecCtx, AVFrame* pFrame, AVPacket* pPacket, FILE* p_output_f)
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

int encode_yuv_to_h264(const char* output_file_path)
{
    // 编码视频时会用到的几个核心对象：
    // AVCodecContext：编码器上下文，保存编码参数和状态
    // AVFrame：原始未压缩图像帧
    // AVPacket：编码后的压缩数据包
    AVCodecContext* pCodecCtx = nullptr;
    const AVCodec* pCodec = nullptr;
    AVPacket* pPacket = nullptr;
    AVFrame* pFrame = nullptr;
    FILE* p_output_f = nullptr;
    const char codec_name[] = "libx264";
    unsigned char endcode[] = { 0x00, 0x00, 0x01, 0x7b };
    int ret = 0;

    output_log(LOG_INFO, "encode_yuv_to_h264 start: %s", output_file_path);

    // 通过名称寻找 H.264 编码器。
    pCodec = avcodec_find_encoder_by_name(codec_name);
    if (!pCodec)
    {
        output_log(LOG_ERROR, "avcodec_find_encoder_by_name error, codec_name=%s", codec_name);
        ret = -1;
        goto end;
    }

    // 分配编码器上下文、压缩包对象、原始帧对象。
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
    // 这些参数决定输出视频的尺寸、码率、帧率、像素格式等。
    pCodecCtx->bit_rate = 400000;
    pCodecCtx->width = 352;
    pCodecCtx->height = 288;
    pCodecCtx->time_base = AVRational{ 1, 25 };
    pCodecCtx->framerate = AVRational{ 25, 1 };
    pCodecCtx->gop_size = 10;
    pCodecCtx->max_b_frames = 1;
    pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (pCodec->id == AV_CODEC_ID_H264)
        av_opt_set(pCodecCtx->priv_data, "preset", "slow", 0);

    // 打开编码器。
    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        output_log(LOG_ERROR, "avcodec_open2 error");
        ret = -1;
        goto end;
    }

    // 设置原始输入帧的参数。
    // 这里告诉 FFmpeg：我接下来送给编码器的原始图像是 YUV420P、352x288。
    pFrame->format = pCodecCtx->pix_fmt;
    pFrame->width = pCodecCtx->width;
    pFrame->height = pCodecCtx->height;

    // 为这一帧图像申请内存，申请成功后 data/linesize 才可用。
    if (av_frame_get_buffer(pFrame, 32) < 0)
    {
        output_log(LOG_ERROR, "av_frame_get_buffer error");
        ret = -1;
        goto end;
    }

    // 打开输出文件，输出的是 H.264 裸流，不是 MP4 封装。
    if (fopen_s(&p_output_f, output_file_path, "wb") != 0 || !p_output_f)
    {
        output_log(LOG_ERROR, "fopen_s error");
        ret = -1;
        goto end;
    }

    // 连续生成 5 秒测试画面。
    // 25fps x 5 秒 = 125 帧。
    for (int i = 0; i < 25 * 5; i++)
    {
        // 确保这一帧的缓冲区当前是可写的。
        if (av_frame_make_writable(pFrame) < 0)
        {
            output_log(LOG_ERROR, "av_frame_make_writable error");
            ret = -1;
            goto end;
        }

        // 构造 Y 分量（亮度）。
        for (int y = 0; y < pCodecCtx->height; y++)
        {
            for (int x = 0; x < pCodecCtx->width; x++)
            {
                pFrame->data[0][y * pFrame->linesize[0] + x] = static_cast<unsigned char>(x + y + i * 3);
            }
        }

        // 构造 U/V 分量（色度）。
        // YUV420P 的 U/V 平面尺寸是宽高的一半，所以这里循环范围也减半。
        for (int y = 0; y < pCodecCtx->height / 2; y++)
        {
            for (int x = 0; x < pCodecCtx->width / 2; x++)
            {
                pFrame->data[1][y * pFrame->linesize[1] + x] = static_cast<unsigned char>(128 + y + i * 2);
                pFrame->data[2][y * pFrame->linesize[2] + x] = static_cast<unsigned char>(64 + x + i * 5);
            }
        }

        // 设置时间戳，编码器会根据它来安排输出帧顺序和时间信息。
        pFrame->pts = i;

        // 把这一帧图像送给编码器。
        ret = encode_video_packet(pCodecCtx, pFrame, pPacket, p_output_f);
        if (ret < 0)
        {
            ret = -1;
            goto end;
        }
    }

    // 刷新编码器，把内部缓存还没吐出来的压缩包全部取出来。
    ret = encode_video_packet(pCodecCtx, nullptr, pPacket, p_output_f);
    if (ret < 0)
    {
        ret = -1;
        goto end;
    }

    // 额外补一个结束码。
    fwrite(endcode, 1, sizeof(endcode), p_output_f);
    output_log(LOG_INFO, "encode_yuv_to_h264 done");

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
