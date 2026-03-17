#pragma once

// 生成一段测试用的 PCM 正弦波，并编码为 MP2 音频文件。
// 这里不依赖真实的 PCM 文件，而是在内存中直接构造 440Hz 的测试音频。
int encode_pcm_to_mp2(const char* output_file_path);
