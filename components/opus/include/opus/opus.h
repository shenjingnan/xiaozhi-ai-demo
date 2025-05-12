#ifndef OPUS_H
#define OPUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码 */
#define OPUS_OK                0
#define OPUS_BAD_ARG          -1
#define OPUS_BUFFER_TOO_SMALL -2
#define OPUS_INTERNAL_ERROR   -3
#define OPUS_INVALID_PACKET   -4
#define OPUS_UNIMPLEMENTED    -5
#define OPUS_INVALID_STATE    -6
#define OPUS_ALLOC_FAIL       -7

/* 应用类型 */
#define OPUS_APPLICATION_VOIP                2048
#define OPUS_APPLICATION_AUDIO               2049
#define OPUS_APPLICATION_RESTRICTED_LOWDELAY 2051

/* CTL 接口 */
#define OPUS_SET_BITRATE(x)          4002
#define OPUS_GET_BITRATE(x)          4003
#define OPUS_SET_COMPLEXITY(x)       4010
#define OPUS_GET_COMPLEXITY(x)       4011

/* 编码器结构体 */
typedef struct OpusEncoder OpusEncoder;

/* 函数声明 */
OpusEncoder *opus_encoder_create(int Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
int opus_encode(OpusEncoder *st, const short *pcm, int frame_size, unsigned char *data, int max_data_bytes);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_H */
