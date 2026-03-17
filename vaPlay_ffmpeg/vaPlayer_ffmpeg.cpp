#include "encode_pcm_to_mp2.h"
#include "encode_yuv_to_h264.h"
#include "vPlayer_sdl2.h"
#include "output_log.h"

#include <cstring>
#include <cstdio>

int main(int argc, char* argv[])
{
    // main 是程序入口，这里只做参数检查和调用真正的播放器逻辑。
    output_log(LOG_INFO, "main argc=%d", argc);
    if (argc > 0 && argv && argv[0])
        output_log(LOG_INFO, "argv[0]=%s", argv[0]);
    if (argc > 1 && argv && argv[1])
        output_log(LOG_INFO, "argv[1]=%s", argv[1]);

    if (argc < 2)
    {
        std::printf("Usage:\n");
        std::printf("  vaPlay_ffmpeg <media-file-path>\n");
        std::printf("  vaPlay_ffmpeg play <media-file-path>\n");
        std::printf("  vaPlay_ffmpeg enc-yuv <output.h264>\n");
        std::printf("  vaPlay_ffmpeg enc-pcm <output.mp2>\n");
        output_log(LOG_ERROR, "missing media-file-path argument");
        return -1;
    }

    // 兼容之前的调用方式：只传一个参数时，默认按“播放视频文件”处理。
    if (argc == 2)
    {
        output_log(LOG_INFO, "calling vPlayer_sdl2");
        return vPlayer_sdl2(argv[1]);
    }

    if (std::strcmp(argv[1], "play") == 0)
        return vPlayer_sdl2(argv[2]);

    if (std::strcmp(argv[1], "enc-yuv") == 0)
        return encode_yuv_to_h264(argv[2]);

    if (std::strcmp(argv[1], "enc-pcm") == 0)
        return encode_pcm_to_mp2(argv[2]);

    std::printf("Unknown command: %s\n", argv[1]);
    return -1;
}
