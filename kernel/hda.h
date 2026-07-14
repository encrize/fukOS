#ifndef HDA_H
#define HDA_H

#include <stdint.h>

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  codec;
    uint8_t  audio_fg;
    uint8_t  dac_node;
    uint8_t  pin_node;
    uint8_t  stream_index;
    uint8_t  volume_percent;
    uint8_t  volume_control;
    uint8_t  repeat_enabled;
    uint8_t  shuffle_enabled;
    uint32_t sample_rate;
} hda_info_t;

int hda_play_pcm16_stereo(const int16_t *pcm, uint32_t bytes,
                          uint32_t sample_rate, hda_info_t *info);

/* Pull-based foreground stream. */
typedef uint32_t (*hda_stream_fill)(void *context, int16_t *dst,
                                    uint32_t max_bytes);
typedef int (*hda_stream_seek)(void *context, int32_t seconds,
                               uint64_t played_bytes);
typedef void (*hda_stream_status)(void *context, uint64_t played_bytes,
                                  int paused, uint8_t volume,
                                  int repeat_enabled, int shuffle_enabled);
int hda_play_stream_48k(hda_stream_fill fill, hda_stream_seek seek,
                        hda_stream_status status, void *context,
                        hda_info_t *info);

/* Cooperatively serviced background stream. */
typedef void (*hda_bg_done)(void *context);
int      hda_bg_start_48k(hda_stream_fill fill, hda_stream_seek seek,
                          hda_bg_done done, void *context, hda_info_t *info);
void     hda_bg_poll(void);
void     hda_bg_stop(void);
void     hda_bg_set_paused(int paused);
int      hda_bg_is_active(void);
int      hda_bg_is_paused(void);
void     hda_bg_set_volume(uint8_t percent);
uint8_t  hda_bg_get_volume(void);
uint64_t hda_bg_played_bytes(void);

/* 0: auto, 1: speakers, 2: headphones. */
void hda_set_output_mode(int mode);
int  hda_get_output_mode(void);
int  hda_headphones_present(void);
int  hda_output_is_headphones(void);
uint8_t hda_output_pin(void);

const char *hda_last_error(void);

#endif
