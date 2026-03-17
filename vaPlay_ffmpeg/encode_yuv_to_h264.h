#pragma once

// 生成一段测试用的 YUV420P 画面，并编码为 H.264 裸流文件。
// 这里不依赖真实的 YUV 文件，而是在内存中直接构造测试图像。
int encode_yuv_to_h264(const char* output_file_path);
