#include "output_log.h"
#include "vPlayer_sdl2.h"

#include <cstring>

#define __STDC_CONSTANT_MACROS
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <SDL3/SDL.h>
}

static int g_frame_rate = 25;   //视频帧率
static int g_sfp_refresh_thread_exit = 0;  //刷新线程是否退出
static int g_sfp_refresh_thread_pause = 0;  //是否暂停播放

// 自定义 SDL 用户事件：
// SFM_REFRESH_EVENT 用来通知主线程“该刷新一帧了”
// SFM_BREAK_EVENT   用来通知主线程“播放结束，可以退出了”
#define SFM_REFRESH_EVENT (SDL_EVENT_USER + 1)
#define SFM_BREAK_EVENT (SDL_EVENT_USER + 2)

typedef struct FFmpeg_V_Param_T
{
    AVFormatContext* pFormatCtx; // 封装格式上下文：负责读文件、找流
    AVCodecContext* pVideoCodecCtx;   // 视频解码器上下文
    AVCodecContext* pAudioCodecCtx;   // 音频解码器上下文
    SwsContext* pSwsCtx;         // 像素格式转换上下文：这里用来转成 YUV420P 供 SDL 显示
    SwrContext* pSwrCtx;         // 音频重采样上下文：把解码后的音频转成 SDL 容易播放的 PCM
    int video_index;             // 视频流下标，一个文件里可能有音频流和视频流
    int audio_index;             // 音频流下标
    AVChannelLayout dst_audio_ch_layout; // SDL 期望的目标声道布局
    AVSampleFormat dst_audio_sample_fmt; // SDL 期望的目标采样格式
    int dst_audio_sample_rate;          // SDL 期望的目标采样率
} FFmpeg_V_Param;

typedef struct SDL_Param_T
{
    SDL_Window* p_sdl_window;     // 播放窗口
    SDL_Renderer* p_sdl_renderer; // 渲染器，负责把纹理画到窗口上
    SDL_Texture* p_sdl_texture;   // 纹理，视频帧数据会更新到这里
    SDL_FRect sdl_rect;           // 目标显示区域
    SDL_Thread* p_sdl_thread;     // 刷新线程，定时向主线程发送“刷新一帧”的事件
    SDL_AudioDeviceID audio_device_id; // SDL 音频设备
    SDL_AudioStream* p_audio_stream;   // SDL 音频流，负责把 PCM 数据送到设备
    SDL_AudioSpec audio_spec;          // 音频播放格式
} SDL_Param;

static void push_sdl_event(Uint32 event_type)
{
    // 构造并压入一个自定义事件，让主线程在事件循环里处理。
    SDL_Event sdl_event;
    SDL_zero(sdl_event);
    sdl_event.type = event_type;
    SDL_PushEvent(&sdl_event);
}

/*
  return value: zero(success) non-zero(failure)
*/
int init_ffmpeg(FFmpeg_V_Param* p_ffmpeg_param, char* filePath)
{
    output_log(LOG_INFO, "init_ffmpeg start: %s", filePath ? filePath : "(null)");

    // 先把结构体清成一个已知状态，方便后面安全释放。
    p_ffmpeg_param->pFormatCtx = nullptr;
    p_ffmpeg_param->pVideoCodecCtx = nullptr;
    p_ffmpeg_param->pAudioCodecCtx = nullptr;
    p_ffmpeg_param->pSwsCtx = nullptr;
    p_ffmpeg_param->pSwrCtx = nullptr;
    p_ffmpeg_param->video_index = -1;
    p_ffmpeg_param->audio_index = -1;
    std::memset(&(p_ffmpeg_param->dst_audio_ch_layout), 0, sizeof(p_ffmpeg_param->dst_audio_ch_layout));
    p_ffmpeg_param->dst_audio_sample_fmt = AV_SAMPLE_FMT_S16;
    p_ffmpeg_param->dst_audio_sample_rate = 44100;
    const AVCodec* pVideoCodec = nullptr;
    const AVCodec* pAudioCodec = nullptr;

    // 初始化网络模块。
    // 即使这里播放的是本地文件，调用它通常也没有坏处。
    avformat_network_init();
    
    

    // 打开输入文件。
    if (avformat_open_input(&(p_ffmpeg_param->pFormatCtx), filePath, nullptr, nullptr) != 0)
    {
        output_log(LOG_ERROR, "avformat_open_input error");
        return -1;
    }

    // 读取媒体文件里的流信息，比如有几个流、编码格式是什么。
    if (avformat_find_stream_info(p_ffmpeg_param->pFormatCtx, nullptr) < 0)
    {
        output_log(LOG_ERROR, "avformat_find_stream_info error");
        return -1;
    }

    // 遍历所有流，找到第一个视频流。
    for (unsigned int i = 0; i < p_ffmpeg_param->pFormatCtx->nb_streams; i++)
    {
        AVStream* pStream = p_ffmpeg_param->pFormatCtx->streams[i];
        if (pStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && p_ffmpeg_param->video_index < 0)
        {
            // 根据视频流里的 codec_id 找到对应的解码器。
            pVideoCodec = avcodec_find_decoder(pStream->codecpar->codec_id);
            if (!pVideoCodec)
            {
                continue;
            }

            // 为这个解码器分配一个上下文对象。
            p_ffmpeg_param->pVideoCodecCtx = avcodec_alloc_context3(pVideoCodec);

            if (!p_ffmpeg_param->pVideoCodecCtx)
            {
                output_log(LOG_ERROR, "avcodec_alloc_context3 error");
                return -1;
            }

            // 把流里的编解码参数拷贝到解码器上下文中。
            if (avcodec_parameters_to_context(p_ffmpeg_param->pVideoCodecCtx, pStream->codecpar) < 0)
            {
                output_log(LOG_ERROR, "avcodec_parameters_to_context error");
                return -1;
            }

            // 从流信息里取平均帧率，后面 SDL 刷新线程会按这个频率推送刷新事件。
            if (pStream->avg_frame_rate.den != 0 && pStream->avg_frame_rate.num != 0)
            {
                g_frame_rate = pStream->avg_frame_rate.num / pStream->avg_frame_rate.den;
            }
            if (g_frame_rate <= 0)
            {
                g_frame_rate = 25;
            }

            p_ffmpeg_param->video_index = static_cast<int>(i);
        }
        else if (pStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && p_ffmpeg_param->audio_index < 0)
        {
            pAudioCodec = avcodec_find_decoder(pStream->codecpar->codec_id);
            if (!pAudioCodec)
            {
                continue;
            }

            p_ffmpeg_param->pAudioCodecCtx = avcodec_alloc_context3(pAudioCodec);
            if (!p_ffmpeg_param->pAudioCodecCtx)
            {
                output_log(LOG_ERROR, "avcodec_alloc_context3(audio) error");
                return -1;
            }

            if (avcodec_parameters_to_context(p_ffmpeg_param->pAudioCodecCtx, pStream->codecpar) < 0)
            {
                output_log(LOG_ERROR, "avcodec_parameters_to_context(audio) error");
                return -1;
            }

            p_ffmpeg_param->audio_index = static_cast<int>(i);
        }
    }

    if (!p_ffmpeg_param->pVideoCodecCtx || !pVideoCodec)
    {
        output_log(LOG_ERROR, "could not find video codec context");
        return -1;
    }

    // 真正打开解码器。
    if (avcodec_open2(p_ffmpeg_param->pVideoCodecCtx, pVideoCodec, nullptr) < 0)
    {
        output_log(LOG_ERROR, "avcodec_open2(video) error");
        return -1;
    }

    // 创建像素格式转换器：
    // 原始视频帧的像素格式不一定是 SDL 最方便显示的格式，
    // 这里统一转成 YUV420P。
    p_ffmpeg_param->pSwsCtx = sws_getContext(p_ffmpeg_param->pVideoCodecCtx->width,
        p_ffmpeg_param->pVideoCodecCtx->height, p_ffmpeg_param->pVideoCodecCtx->pix_fmt,
        p_ffmpeg_param->pVideoCodecCtx->width, p_ffmpeg_param->pVideoCodecCtx->height,
        AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);

    if (!p_ffmpeg_param->pSwsCtx)
    {
        output_log(LOG_INFO, "sws_getContext error");
        return -1;
    }

    if (p_ffmpeg_param->pAudioCodecCtx && pAudioCodec)
    {
        if (avcodec_open2(p_ffmpeg_param->pAudioCodecCtx, pAudioCodec, nullptr) < 0)
        {
            output_log(LOG_ERROR, "avcodec_open2(audio) error");
            return -1;
        }

        av_channel_layout_default(&(p_ffmpeg_param->dst_audio_ch_layout), 2);
        if (swr_alloc_set_opts2(&(p_ffmpeg_param->pSwrCtx),
            &(p_ffmpeg_param->dst_audio_ch_layout),
            p_ffmpeg_param->dst_audio_sample_fmt,
            p_ffmpeg_param->dst_audio_sample_rate,
            &(p_ffmpeg_param->pAudioCodecCtx->ch_layout),
            p_ffmpeg_param->pAudioCodecCtx->sample_fmt,
            p_ffmpeg_param->pAudioCodecCtx->sample_rate,
            0, nullptr) < 0)
        {
            output_log(LOG_ERROR, "swr_alloc_set_opts2 error");
            return -1;
        }

        if (swr_init(p_ffmpeg_param->pSwrCtx) < 0)
        {
            output_log(LOG_ERROR, "swr_init error");
            return -1;
        }
    }

    output_log(LOG_INFO, "init_ffmpeg ok: width=%d height=%d fps=%d audio=%s",
        p_ffmpeg_param->pVideoCodecCtx->width, p_ffmpeg_param->pVideoCodecCtx->height, g_frame_rate,
        p_ffmpeg_param->audio_index >= 0 ? "on" : "off");

    av_dump_format(p_ffmpeg_param->pFormatCtx, p_ffmpeg_param->video_index, filePath, 0);  //output I/O format detail Info debug
    return 0;
}

/*
  return value: zero(success) non-zero(failure)
*/
int release_ffmpeg(FFmpeg_V_Param* p_ffmpeg_param)
{
    if (!p_ffmpeg_param)
        return -1;

    // 释放顺序大体遵循“谁申请谁释放”的原则。
    if (p_ffmpeg_param->pSwsCtx)
        sws_freeContext(p_ffmpeg_param->pSwsCtx);

    if (p_ffmpeg_param->pSwrCtx)
        swr_free(&(p_ffmpeg_param->pSwrCtx));

    if (p_ffmpeg_param->pVideoCodecCtx)
        avcodec_free_context(&(p_ffmpeg_param->pVideoCodecCtx));

    if (p_ffmpeg_param->pAudioCodecCtx)
        avcodec_free_context(&(p_ffmpeg_param->pAudioCodecCtx));

    if (p_ffmpeg_param->pFormatCtx)
        avformat_close_input(&(p_ffmpeg_param->pFormatCtx));

    av_channel_layout_uninit(&(p_ffmpeg_param->dst_audio_ch_layout));

    delete p_ffmpeg_param;
    return 0;
}

int sfp_refresh_thread(void* opaque)
{
    (void)opaque;
    g_sfp_refresh_thread_exit = 0;
    g_sfp_refresh_thread_pause = 0;

    // SDL 的渲染通常放在主线程，这个线程只负责“定时发消息”。
    while (!g_sfp_refresh_thread_exit)
    {
        if (!g_sfp_refresh_thread_pause)
        {
            push_sdl_event(SFM_REFRESH_EVENT);
        }
        SDL_Delay(1000 / g_frame_rate);//每隔 (1000 / g_frame_rate )ms push (SFM_REFRESH_EVENT)
    }

    g_sfp_refresh_thread_exit = 0;
    g_sfp_refresh_thread_pause = 0;
    push_sdl_event(SFM_BREAK_EVENT);
    return 0;
}

static int push_audio_frame(FFmpeg_V_Param* p_ffmpeg_param, SDL_Param_T* p_sdl_param, AVFrame* p_audio_frame)
{
    if (!p_ffmpeg_param || !p_sdl_param || !p_audio_frame || !p_ffmpeg_param->pSwrCtx || !p_sdl_param->p_audio_stream)
        return -1;

    const int dst_channels = p_ffmpeg_param->dst_audio_ch_layout.nb_channels;
    const int dst_nb_samples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(p_ffmpeg_param->pSwrCtx, p_ffmpeg_param->pAudioCodecCtx->sample_rate) + p_audio_frame->nb_samples,
        p_ffmpeg_param->dst_audio_sample_rate,
        p_ffmpeg_param->pAudioCodecCtx->sample_rate,
        AV_ROUND_UP));

    uint8_t* out_buffer = nullptr;
    int out_linesize = 0;
    if (av_samples_alloc(&out_buffer, &out_linesize, dst_channels, dst_nb_samples,
        p_ffmpeg_param->dst_audio_sample_fmt, 0) < 0)
    {
        output_log(LOG_ERROR, "av_samples_alloc(audio) error");
        return -1;
    }

    const int converted_samples = swr_convert(p_ffmpeg_param->pSwrCtx, &out_buffer, dst_nb_samples,
        (const uint8_t**)p_audio_frame->extended_data, p_audio_frame->nb_samples);
    if (converted_samples < 0)
    {
        output_log(LOG_ERROR, "swr_convert error");
        av_freep(&out_buffer);
        return -1;
    }

    const int out_buffer_size = av_samples_get_buffer_size(&out_linesize, dst_channels, converted_samples,
        p_ffmpeg_param->dst_audio_sample_fmt, 1);
    if (out_buffer_size < 0)
    {
        output_log(LOG_ERROR, "av_samples_get_buffer_size(audio) error");
        av_freep(&out_buffer);
        return -1;
    }

    if (!SDL_PutAudioStreamData(p_sdl_param->p_audio_stream, out_buffer, out_buffer_size))
    {
        output_log(LOG_ERROR, "SDL_PutAudioStreamData error: %s", SDL_GetError());
        av_freep(&out_buffer);
        return -1;
    }

    av_freep(&out_buffer);
    return 0;
}

static int decode_audio_packet(FFmpeg_V_Param* p_ffmpeg_param, SDL_Param_T* p_sdl_param, AVPacket* packet, AVFrame* p_audio_frame)
{
    if (!p_ffmpeg_param->pAudioCodecCtx || !p_sdl_param->p_audio_stream)
        return 0;

    if (avcodec_send_packet(p_ffmpeg_param->pAudioCodecCtx, packet) < 0)
    {
        output_log(LOG_ERROR, "avcodec_send_packet(audio) error");
        return -1;
    }

    while (true)
    {
        const int ret = avcodec_receive_frame(p_ffmpeg_param->pAudioCodecCtx, p_audio_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
        {
            output_log(LOG_ERROR, "avcodec_receive_frame(audio) error");
            return -1;
        }

        if (push_audio_frame(p_ffmpeg_param, p_sdl_param, p_audio_frame) < 0)
            return -1;

        av_frame_unref(p_audio_frame);
    }

    return 0;
}

int init_sdl2(SDL_Param_T* p_sdl_param, FFmpeg_V_Param* p_ffmpeg_param, int screen_w, int screen_h)
{
    output_log(LOG_INFO, "init_sdl start: width=%d height=%d", screen_w, screen_h);

    // 初始化 SDL 的音频和视频子系统。
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO))
    {
        output_log(LOG_ERROR, "SDL_Init error: %s", SDL_GetError());
        return -1;
    }

    // 一次性创建窗口和渲染器。
    if (!SDL_CreateWindowAndRenderer("vPlayer_sdl", screen_w, screen_h, 0,
        &(p_sdl_param->p_sdl_window), &(p_sdl_param->p_sdl_renderer)))
    {
        output_log(LOG_ERROR, "SDL_CreateWindowAndRenderer error: %s", SDL_GetError());
        return -1;
    }

    // 创建一块 IYUV(YUV420P) 纹理，后续每一帧视频都更新到这块纹理里。
    p_sdl_param->p_sdl_texture = SDL_CreateTexture(p_sdl_param->p_sdl_renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, screen_w, screen_h);
    if (!p_sdl_param->p_sdl_texture)
    {
        output_log(LOG_ERROR, "SDL_CreateTexture error: %s", SDL_GetError());
        return -1;
    }

    p_sdl_param->sdl_rect.x = 0.0f;
    p_sdl_param->sdl_rect.y = 0.0f;
    p_sdl_param->sdl_rect.w = static_cast<float>(screen_w);
    p_sdl_param->sdl_rect.h = static_cast<float>(screen_h);
    p_sdl_param->audio_device_id = 0;
    p_sdl_param->p_audio_stream = nullptr;
    std::memset(&(p_sdl_param->audio_spec), 0, sizeof(p_sdl_param->audio_spec));

    if (p_ffmpeg_param && p_ffmpeg_param->audio_index >= 0)
    {
        p_sdl_param->audio_spec.format = SDL_AUDIO_S16;
        p_sdl_param->audio_spec.channels = p_ffmpeg_param->dst_audio_ch_layout.nb_channels;
        p_sdl_param->audio_spec.freq = p_ffmpeg_param->dst_audio_sample_rate;

        p_sdl_param->audio_device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &(p_sdl_param->audio_spec));
        if (p_sdl_param->audio_device_id == 0)
        {
            output_log(LOG_ERROR, "SDL_OpenAudioDevice error: %s", SDL_GetError());
            return -1;
        }

        p_sdl_param->p_audio_stream = SDL_CreateAudioStream(&(p_sdl_param->audio_spec), &(p_sdl_param->audio_spec));
        if (!p_sdl_param->p_audio_stream)
        {
            output_log(LOG_ERROR, "SDL_CreateAudioStream error: %s", SDL_GetError());
            return -1;
        }

        if (!SDL_BindAudioStream(p_sdl_param->audio_device_id, p_sdl_param->p_audio_stream))
        {
            output_log(LOG_ERROR, "SDL_BindAudioStream error: %s", SDL_GetError());
            return -1;
        }

        if (!SDL_ResumeAudioDevice(p_sdl_param->audio_device_id))
        {
            output_log(LOG_ERROR, "SDL_ResumeAudioDevice error: %s", SDL_GetError());
            return -1;
        }
    }

    // 启动刷新线程，它会按帧率投递 SFM_REFRESH_EVENT 事件。
    p_sdl_param->p_sdl_thread = SDL_CreateThread(sfp_refresh_thread, "refresh_thread", nullptr);
    if (!p_sdl_param->p_sdl_thread)
    {
        output_log(LOG_ERROR, "SDL_CreateThread error: %s", SDL_GetError());
        return -1;
    }

    output_log(LOG_INFO, "init_sdl ok");
    return 0;
}

int release_sdl2(SDL_Param_T* p_sdl_param)
{
    if (!p_sdl_param)
        return -1;

    // 等待刷新线程结束，避免线程还在跑时窗口对象已经被销毁。
    if (p_sdl_param->p_sdl_thread)
    {
        SDL_WaitThread(p_sdl_param->p_sdl_thread, nullptr);
    }

    if (p_sdl_param->p_sdl_texture)
        SDL_DestroyTexture(p_sdl_param->p_sdl_texture);
    if (p_sdl_param->p_audio_stream)
        SDL_DestroyAudioStream(p_sdl_param->p_audio_stream);
    if (p_sdl_param->audio_device_id != 0)
        SDL_CloseAudioDevice(p_sdl_param->audio_device_id);
    if (p_sdl_param->p_sdl_renderer)
        SDL_DestroyRenderer(p_sdl_param->p_sdl_renderer);
    if (p_sdl_param->p_sdl_window)
        SDL_DestroyWindow(p_sdl_param->p_sdl_window);

    SDL_Quit();
    delete p_sdl_param;
    return 0;
}

int vPlayer_sdl2(char* filePath)
{
    output_log(LOG_INFO, "vPlayer_sdl2 start");

    // FFmpeg 相关对象。
    FFmpeg_V_Param* p_ffmpeg_param = nullptr;
    AVPacket* packet = nullptr;   // 压缩包，一般对应读取到的一段码流数据
    AVFrame* pFrame = nullptr;    // 解码后的原始视频帧
    AVFrame* pFrameYUV = nullptr; // 转成 YUV420P 后，准备用来显示的视频帧
    AVFrame* pAudioFrame = nullptr; // 解码后的原始音频帧
    int out_buffer_size = 0;
    unsigned char* out_buffer = nullptr;

    // SDL 相关对象。
    SDL_Param_T* p_sdl_param = nullptr;
    SDL_Event sdl_event;

    int ret = 0;

    // 1. 初始化 FFmpeg。
    p_ffmpeg_param = new FFmpeg_V_Param();
    std::memset(p_ffmpeg_param, 0, sizeof(FFmpeg_V_Param));
    if (init_ffmpeg(p_ffmpeg_param, filePath))
    {
        ret = -1;
        goto end;
    }

    // 2. 申请 packet 和 frame。
    packet = av_packet_alloc();
    pFrame = av_frame_alloc();
    pFrameYUV = av_frame_alloc();
    pAudioFrame = av_frame_alloc();
    if (!packet || !pFrame || !pFrameYUV || !pAudioFrame)
    {
        output_log(LOG_ERROR, "alloc frame or packet failed");
        ret = -1;
        goto end;
    }

    // 3. 为目标 YUV420P 图像申请一块连续内存。
    out_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
        p_ffmpeg_param->pVideoCodecCtx->width, p_ffmpeg_param->pVideoCodecCtx->height, 1);
    out_buffer = static_cast<unsigned char*>(av_malloc(out_buffer_size));
    if (!out_buffer)
    {
        output_log(LOG_ERROR, "av_malloc out_buffer failed");
        ret = -1;
        goto end;
    }

    // 把申请到的内存挂到 pFrameYUV 的 data/linesize 上，
    // 这样后面 sws_scale 就能把转换后的图像写进来。
    if (av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer,
        AV_PIX_FMT_YUV420P, p_ffmpeg_param->pVideoCodecCtx->width, p_ffmpeg_param->pVideoCodecCtx->height, 1) < 0)
    {
        output_log(LOG_ERROR, "av_image_fill_arrays error");
        ret = -1;
        goto end;
    }

    // 4. 初始化 SDL，准备窗口和渲染器。
    p_sdl_param = new SDL_Param_T();
    std::memset(p_sdl_param, 0, sizeof(SDL_Param_T));
    if (init_sdl2(p_sdl_param, p_ffmpeg_param, p_ffmpeg_param->pVideoCodecCtx->width, p_ffmpeg_param->pVideoCodecCtx->height))
    {
        ret = -1;
        goto end;
    }

    output_log(LOG_INFO, "enter event loop");

    // 5. 主循环：
    //    SDL 刷新线程会定时塞入一个“刷新一帧”的事件；
    //    主线程收到后读取一个 packet，送给解码器，再把解码得到的 frame 显示出来。
    while (true)
    {
        int temp_ret = 0;
        if (!SDL_WaitEvent(&sdl_event))
        {
            output_log(LOG_ERROR, "SDL_WaitEvent error: %s", SDL_GetError());
            ret = -1;
            break;
        }

        if (sdl_event.type == SFM_REFRESH_EVENT)
        {
            // 持续读包，直到拿到一个视频包为止。
            while (true)
            {
                if (av_read_frame(p_ffmpeg_param->pFormatCtx, packet) < 0)
                {
                    // 读不到包通常意味着到文件末尾了。
                    g_sfp_refresh_thread_exit = 1;
                    break;
                }
                if (packet->stream_index == p_ffmpeg_param->audio_index)
                {
                    if (decode_audio_packet(p_ffmpeg_param, p_sdl_param, packet, pAudioFrame) < 0)
                    {
                        g_sfp_refresh_thread_exit = 1;
                        ret = -1;
                        break;
                    }
                    av_packet_unref(packet);
                    continue;
                }
                if (packet->stream_index == p_ffmpeg_param->video_index)
                {
                    break;
                }
                av_packet_unref(packet);
            }

            if (packet->stream_index != p_ffmpeg_param->video_index)
            {
                continue;
            }

            if (avcodec_send_packet(p_ffmpeg_param->pVideoCodecCtx, packet) < 0)
            {
                g_sfp_refresh_thread_exit = 1;
            }

            // 一个 packet 可能解出 0 帧、1 帧甚至多帧，所以这里循环取 frame。
            do
            {
                temp_ret = avcodec_receive_frame(p_ffmpeg_param->pVideoCodecCtx, pFrame);
                if (temp_ret == AVERROR_EOF)
                {
                    g_sfp_refresh_thread_exit = 1;
                    break;
                }
                if (temp_ret == 0)
                {
                    // 把原始像素格式转成 SDL 纹理更容易显示的 YUV420P。
                    sws_scale(p_ffmpeg_param->pSwsCtx, (const unsigned char* const*)pFrame->data,
                        pFrame->linesize, 0, p_ffmpeg_param->pVideoCodecCtx->height, pFrameYUV->data,
                        pFrameYUV->linesize);

                    // 用 Y/U/V 三个平面更新 SDL 纹理。
                    SDL_UpdateYUVTexture(p_sdl_param->p_sdl_texture, nullptr,
                        pFrameYUV->data[0], pFrameYUV->linesize[0],
                        pFrameYUV->data[1], pFrameYUV->linesize[1],
                        pFrameYUV->data[2], pFrameYUV->linesize[2]);

                    // 清空窗口 -> 复制纹理 -> 显示到屏幕。
                    SDL_RenderClear(p_sdl_param->p_sdl_renderer);
                    SDL_RenderTexture(p_sdl_param->p_sdl_renderer, p_sdl_param->p_sdl_texture,
                        nullptr, &(p_sdl_param->sdl_rect));
                    SDL_RenderPresent(p_sdl_param->p_sdl_renderer);
                    av_frame_unref(pFrame);
                }
            } while (temp_ret != AVERROR(EAGAIN));

            // packet 用完后一定要 unref，不然内部引用不会释放。
            av_packet_unref(packet);
        }
        else if (sdl_event.type == SFM_BREAK_EVENT)
        {
            break;
        }
        else if (sdl_event.type == SDL_EVENT_KEY_DOWN)
        {
            // 空格暂停/继续，Q 退出。
            if (sdl_event.key.key == SDLK_SPACE)
            {
                g_sfp_refresh_thread_pause = !g_sfp_refresh_thread_pause;
                if (p_sdl_param->audio_device_id != 0)
                {
                    if (g_sfp_refresh_thread_pause)
                        SDL_PauseAudioDevice(p_sdl_param->audio_device_id);
                    else
                        SDL_ResumeAudioDevice(p_sdl_param->audio_device_id);
                }
            }
            if (sdl_event.key.key == SDLK_Q)
                g_sfp_refresh_thread_exit = 1;
        }
        else if (sdl_event.type == SDL_EVENT_QUIT)
        {
            g_sfp_refresh_thread_exit = 1;
        }
    }

end:
    output_log(LOG_INFO, "vPlayer_sdl2 end ret=%d", ret);

    // 6. 统一释放资源。
    if (out_buffer)
        av_free(out_buffer);
    if (packet)
        av_packet_free(&packet);
    if (pFrame)
        av_frame_free(&pFrame);
    if (pFrameYUV)
        av_frame_free(&pFrameYUV);
    if (pAudioFrame)
        av_frame_free(&pAudioFrame);
    if (p_ffmpeg_param)
        release_ffmpeg(p_ffmpeg_param);
    if (p_sdl_param)
        release_sdl2(p_sdl_param);
    return ret;
}
