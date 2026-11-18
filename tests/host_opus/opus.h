// HOST TEST SHIM ONLY: the real Opus headers aren't installed on the test machine, only libopus.so.0.
// These are the stable public prototypes/constants of libopus. Android builds use the real <opus.h>.
#pragma once
#include <cstdint>
extern "C" {
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
typedef std::int32_t opus_int32;
typedef std::int16_t opus_int16;
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002
#define OPUS_SET_COMPLEXITY_REQUEST 4010
#define OPUS_SET_INBAND_FEC_REQUEST 4012
#define OPUS_SET_BITRATE(x) OPUS_SET_BITRATE_REQUEST, (opus_int32)(x)
#define OPUS_SET_COMPLEXITY(x) OPUS_SET_COMPLEXITY_REQUEST, (opus_int32)(x)
#define OPUS_SET_INBAND_FEC(x) OPUS_SET_INBAND_FEC_REQUEST, (opus_int32)(x)
OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
int opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);
OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *st);
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec);
}
