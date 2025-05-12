#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 定义音频参数
#define SAMPLE_RATE 16000  // 采样率 16kHz
#define CHANNELS 1         // 单声道
#define FRAME_SIZE 320     // 每帧采样点数 (20ms @ 16kHz)
#define MAX_PACKET_SIZE 1500  // 最大Opus包大小

// 简单的PCM数据生成函数
static short* generate_pcm_from_mp3(const char* filename, int* pcm_len) {
    printf("读取MP3文件: %s\n", filename);
    
    // 打开MP3文件
    FILE* mp3_file = fopen(filename, "rb");
    if (mp3_file == NULL) {
        printf("无法打开MP3文件: %s\n", filename);
        return NULL;
    }
    
    // 获取文件大小
    fseek(mp3_file, 0, SEEK_END);
    long file_size = ftell(mp3_file);
    fseek(mp3_file, 0, SEEK_SET);
    
    printf("MP3文件大小: %ld 字节\n", file_size);
    
    // 分配内存读取整个文件
    unsigned char* mp3_data = (unsigned char*)malloc(file_size);
    if (mp3_data == NULL) {
        printf("内存分配失败\n");
        fclose(mp3_file);
        return NULL;
    }
    
    // 读取文件内容
    size_t bytes_read = fread(mp3_data, 1, file_size, mp3_file);
    fclose(mp3_file);
    
    if (bytes_read != file_size) {
        printf("读取文件失败，预期: %ld 字节，实际: %zu 字节\n", file_size, bytes_read);
        free(mp3_data);
        return NULL;
    }
    
    // 生成PCM数据（简化处理，实际应使用MP3解码器）
    *pcm_len = SAMPLE_RATE * 5;  // 5秒的音频
    short* pcm_data = (short*)malloc(*pcm_len * sizeof(short));
    if (pcm_data == NULL) {
        printf("PCM内存分配失败\n");
        free(mp3_data);
        return NULL;
    }
    
    // 生成一些示例PCM数据
    for (int i = 0; i < *pcm_len; i++) {
        // 使用MP3文件的前几个字节来影响生成的波形
        float freq = 0.01 + (mp3_data[i % 100] / 1000.0);
        pcm_data[i] = (short)(sin(i * freq) * 10000);
    }
    
    free(mp3_data);
    printf("生成PCM数据完成，长度: %d 采样点\n", *pcm_len);
    
    return pcm_data;
}

// 简单的Opus编码模拟函数
static int encode_pcm_to_opus(const short* pcm_data, int pcm_len, const char* opus_filename) {
    printf("开始Opus编码...\n");
    
    // 计算帧数
    int num_frames = pcm_len / FRAME_SIZE;
    printf("PCM数据长度: %d 采样点, 帧数: %d\n", pcm_len, num_frames);
    
    // 分配内存存储所有编码后的数据
    size_t max_opus_size = num_frames * MAX_PACKET_SIZE;
    unsigned char* all_opus_data = (unsigned char*)malloc(max_opus_size);
    if (all_opus_data == NULL) {
        printf("内存分配失败\n");
        return -1;
    }
    
    // 模拟编码过程
    size_t total_bytes = 0;
    for (int i = 0; i < num_frames; i++) {
        // 获取当前帧的PCM数据
        const short* frame = &pcm_data[i * FRAME_SIZE];
        
        // 模拟编码（简化处理，实际应使用Opus编码器）
        // 这里我们简单地将每个PCM样本压缩为8位
        int nbytes = FRAME_SIZE / 2;  // 压缩比2:1
        
        // 存储包长度
        uint16_t packet_len = (uint16_t)nbytes;
        memcpy(all_opus_data + total_bytes, &packet_len, sizeof(uint16_t));
        total_bytes += sizeof(uint16_t);
        
        // 存储包数据（简化处理）
        for (int j = 0; j < nbytes; j++) {
            all_opus_data[total_bytes + j] = (unsigned char)((frame[j*2] >> 8) + 128);
        }
        total_bytes += nbytes;
        
        // 打印进度
        if (i % 100 == 0) {
            printf("已编码 %d/%d 帧\n", i, num_frames);
        }
    }
    
    // 打印编码结果
    printf("Opus编码完成，总共编码 %d 帧，总大小 %zu 字节\n", num_frames, total_bytes);
    printf("压缩比: %.2f%%\n", (float)total_bytes * 100 / (pcm_len * sizeof(short)));
    
    // 保存编码后的数据到文件
    FILE* opus_file = fopen(opus_filename, "wb");
    if (opus_file == NULL) {
        printf("无法创建输出文件: %s\n", opus_filename);
        free(all_opus_data);
        return -1;
    }
    
    // 写入总大小
    fwrite(&total_bytes, sizeof(size_t), 1, opus_file);
    // 写入所有编码后的数据
    fwrite(all_opus_data, 1, total_bytes, opus_file);
    fclose(opus_file);
    
    printf("已保存Opus编码数据到文件: %s\n", opus_filename);
    
    free(all_opus_data);
    return 0;
}

int main(int argc, char* argv[]) {
    const char* mp3_filename = "打开客厅灯.mp3";
    const char* opus_filename = "打开客厅灯.opus";
    
    // 如果命令行提供了文件名，则使用命令行参数
    if (argc > 1) {
        mp3_filename = argv[1];
    }
    if (argc > 2) {
        opus_filename = argv[2];
    }
    
    printf("测试Opus编码: %s -> %s\n", mp3_filename, opus_filename);
    
    // 读取MP3文件并生成PCM数据
    int pcm_len = 0;
    short* pcm_data = generate_pcm_from_mp3(mp3_filename, &pcm_len);
    if (pcm_data == NULL) {
        printf("生成PCM数据失败\n");
        return 1;
    }
    
    // 编码PCM数据为Opus格式
    int ret = encode_pcm_to_opus(pcm_data, pcm_len, opus_filename);
    
    // 释放PCM数据
    free(pcm_data);
    
    if (ret != 0) {
        printf("Opus编码失败\n");
        return 1;
    }
    
    printf("Opus编码测试完成\n");
    return 0;
}
