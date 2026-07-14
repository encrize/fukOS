#include <stdint.h>
#include "hda.h"
#include "pci.h"
#include "io.h"
#include "keyboard.h"
#include "util.h"

#define HDA_GCAP       0x00u
#define HDA_GCTL       0x08u
#define HDA_STATESTS   0x0Eu
#define HDA_ICOI       0x60u
#define HDA_ICII       0x64u
#define HDA_ICIS       0x68u
#define HDA_SD_BASE    0x80u
#define HDA_SD_SIZE    0x20u
#define SD_CTL         0x00u
#define SD_STS         0x03u
#define SD_LPIB        0x04u
#define SD_CBL         0x08u
#define SD_LVI         0x0Cu
#define SD_FMT         0x12u
#define SD_BDPL        0x18u
#define SD_BDPU        0x1Cu

#define PARAM_NODE_COUNT       0x04u
#define PARAM_FUNCTION_TYPE    0x05u
#define PARAM_AUDIO_WIDGET_CAP 0x09u
#define PARAM_PIN_CAP          0x0Cu
#define PARAM_CONN_LIST_LEN    0x0Eu
#define PARAM_OUTPUT_AMP_CAP   0x12u

#define MAX_BDL 256u
#define BDL_CHUNK (1024u * 1024u)
#define MAX_NODES 256u

typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t flags;
} hda_bdle_t;

static hda_bdle_t g_bdl[MAX_BDL] __attribute__((aligned(128)));
#define STREAM_BLOCK_BYTES (256u * 1024u)
#define STREAM_BLOCKS 4u
static uint8_t g_stream_ring[STREAM_BLOCKS][STREAM_BLOCK_BYTES]
    __attribute__((aligned(128)));
static uint32_t g_widget_caps[MAX_NODES];
static uint8_t g_seen[MAX_NODES];
static volatile uint8_t *g_mmio;
static uint8_t g_codec;
static const char *g_error = "no error";
static uint8_t g_volume_percent = 70u;
static int g_repeat_enabled;
static int g_shuffle_enabled;
static int g_output_mode;
static int g_last_hp_present;
static int g_last_output_hp;
static uint8_t g_last_output_pin;

static uint8_t r8(uint32_t o) { return *(volatile uint8_t *)(g_mmio + o); }
static uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(g_mmio + o); }
static void w8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(g_mmio + o) = v; }
static void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }

static uint64_t tsc_per_ms(void) {
    uint32_t maxleaf, b, c, d;
    cpuid_query(0, &maxleaf, &b, &c, &d);
    if (maxleaf >= 0x15u) {
        uint32_t den, num, crystal;
        cpuid_query(0x15u, &den, &num, &crystal, &d);
        if (den && num) {
            if (!crystal) crystal = 19200000u;
            return ((uint64_t)crystal * num) / ((uint64_t)den * 1000u);
        }
    }
    return 1100000u;
}

static void delay_ms(uint32_t ms) {
    uint64_t until = rdtsc() + tsc_per_ms() * ms;
    while ((int64_t)(rdtsc() - until) < 0) { }
}

static int wait32(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
    uint64_t until = rdtsc() + tsc_per_ms() * ms;
    do {
        if ((r32(off) & mask) == want) return 1;
    } while ((int64_t)(rdtsc() - until) < 0);
    return 0;
}

static int wait8(uint32_t off, uint8_t mask, uint8_t want, uint32_t ms) {
    uint64_t until = rdtsc() + tsc_per_ms() * ms;
    do {
        if ((r8(off) & mask) == want) return 1;
    } while ((int64_t)(rdtsc() - until) < 0);
    return 0;
}

static int codec_cmd(uint32_t cmd, uint32_t *response) {
    uint64_t until = rdtsc() + tsc_per_ms() * 100u;
    while (r16(HDA_ICIS) & 1u) {
        if ((int64_t)(rdtsc() - until) >= 0) return 0;
    }
    w16(HDA_ICIS, 2u);
    w32(HDA_ICOI, cmd);
    w16(HDA_ICIS, 1u);
    until = rdtsc() + tsc_per_ms() * 100u;
    while (!(r16(HDA_ICIS) & 2u)) {
        if ((int64_t)(rdtsc() - until) >= 0) return 0;
    }
    if (response) *response = r32(HDA_ICII);
    w16(HDA_ICIS, 2u);
    return 1;
}

static int verb12(uint8_t nid, uint16_t verb, uint8_t payload, uint32_t *resp) {
    uint32_t cmd = ((uint32_t)g_codec << 28) | ((uint32_t)nid << 20)
                 | ((uint32_t)verb << 8) | payload;
    return codec_cmd(cmd, resp);
}

static int verb4(uint8_t nid, uint8_t verb, uint16_t payload, uint32_t *resp) {
    uint32_t cmd = ((uint32_t)g_codec << 28) | ((uint32_t)nid << 20)
                 | ((uint32_t)verb << 16) | payload;
    return codec_cmd(cmd, resp);
}

static uint32_t get_param(uint8_t nid, uint8_t param) {
    uint32_t v = 0;
    (void)verb12(nid, 0xF00u, param, &v);
    return v;
}

static uint32_t output_amp_caps(uint8_t nid, uint8_t afg) {
    uint32_t wc = g_widget_caps[nid];
    if (!(wc & (1u << 2))) return 0;
    uint8_t owner = (wc & (1u << 3)) ? nid : afg;
    uint32_t caps = get_param(owner, PARAM_OUTPUT_AMP_CAP);
    if (!caps && owner != nid) caps = get_param(nid, PARAM_OUTPUT_AMP_CAP);
    return caps;
}

static void set_amp_gain(uint8_t nid, uint32_t caps, uint8_t percent) {
    if (!caps) return;
    uint8_t zero_db = (uint8_t)(caps & 0x7fu);
    uint8_t gain = percent ? (uint8_t)(((uint32_t)zero_db * percent) / 100u) : 0u;
    uint16_t payload = (uint16_t)(0xB000u | gain);
    if (!percent) payload |= 0x0080u;
    (void)verb4(nid, 0x3u, payload, 0);
}

static int get_connection(uint8_t nid, uint8_t index, uint8_t *out) {
    uint32_t lenp = get_param(nid, PARAM_CONN_LIST_LEN);
    uint8_t len = (uint8_t)(lenp & 0x7Fu);
    int long_form = (lenp & 0x80u) != 0;
    if (index >= len) return 0;
    uint8_t per = long_form ? 2u : 4u;
    uint8_t base = (uint8_t)((index / per) * per);
    uint32_t list = 0;
    if (!verb12(nid, 0xF02u, base, &list)) return 0;
    if (long_form)
        *out = (uint8_t)((list >> ((index & 1u) * 16u)) & 0x7Fu);
    else
        *out = (uint8_t)((list >> ((index & 3u) * 8u)) & 0x7Fu);
    return *out != 0;
}

static int route_to_dac(uint8_t nid, uint8_t depth, uint8_t *dac) {
    if (!nid || depth > 16u || g_seen[nid]) return 0;
    g_seen[nid] = 1;
    uint8_t type = (uint8_t)((g_widget_caps[nid] >> 20) & 0x0Fu);
    if (type == 0u) { *dac = nid; return 1; }

    uint8_t len = (uint8_t)(get_param(nid, PARAM_CONN_LIST_LEN) & 0x7Fu);
    for (uint8_t i = 0; i < len; i++) {
        uint8_t child;
        if (!get_connection(nid, i, &child)) continue;
        if (route_to_dac(child, (uint8_t)(depth + 1u), dac)) {
            (void)verb12(nid, 0x701u, i, 0);
            return 1;
        }
    }
    return 0;
}

/* Convert common PCM rates to the HDA stream-format encoding. */
static uint16_t make_format(uint32_t rate) {
    const uint32_t bases[2] = { 48000u, 44100u };
    for (uint32_t b = 0; b < 2u; b++) {
        for (uint32_t mul = 1u; mul <= 4u; mul++) {
            for (uint32_t div = 1u; div <= 8u; div++) {
                if (bases[b] * mul == rate * div) {
                    return (uint16_t)((b ? (1u << 14) : 0u)
                           | ((mul - 1u) << 11) | ((div - 1u) << 8)
                           | (1u << 4)   | 1u  );
                }
            }
        }
    }
    return 0xFFFFu;
}

static int headphone_present(uint8_t nid) {
    uint32_t pincap = get_param(nid, PARAM_PIN_CAP);
    if (!(pincap & (1u << 2))) return 0;
    (void)verb12(nid, 0x709u, 0u, 0);
    uint32_t sense = 0;
    if (!verb12(nid, 0xF09u, 0u, &sense)) return 0;
    return (sense & (1u << 31)) != 0;
}

static int init_controller(pci_dev_t *dev, uint8_t *audio_fg,
                           uint8_t *pin, uint8_t *dac, uint8_t *stream) {
    if (!pci_find_class(0x04u, 0x03u, 0xFFu, dev) &&
        !pci_find_class(0x04u, 0x01u, 0xFFu, dev)) {
        g_error = "HDA PCI controller not found (run lspci)";
        return 0;
    }
    uint64_t bar = pci_bar_address(dev->bus, dev->slot, dev->func, 0);
    if (!bar || (bar >> 32)) {
        g_error = "HDA BAR0 is empty or above 4 GiB";
        return 0;
    }
    pci_enable_device(dev->bus, dev->slot, dev->func);
    g_mmio = (volatile uint8_t *)(uintptr_t)(uint32_t)bar;

    w32(HDA_GCTL, r32(HDA_GCTL) & ~1u);
    if (!wait32(HDA_GCTL, 1u, 0u, 100u)) {
        g_error = "HDA controller reset-low timeout";
        return 0;
    }
    w32(HDA_GCTL, r32(HDA_GCTL) | 1u);
    if (!wait32(HDA_GCTL, 1u, 1u, 100u)) {
        g_error = "HDA controller reset-high timeout";
        return 0;
    }
    delay_ms(2u);

    uint16_t codecs = r16(HDA_STATESTS);
    if (!codecs) {
        g_error = "HDA controller has no responding codec";
        return 0;
    }
    g_codec = 0;
    while (g_codec < 15u && !(codecs & (1u << g_codec))) g_codec++;

    uint32_t root_nodes = get_param(0, PARAM_NODE_COUNT);
    uint8_t root_start = (uint8_t)(root_nodes >> 16);
    uint8_t root_count = (uint8_t)root_nodes;
    *audio_fg = 0;
    for (uint8_t i = 0; i < root_count; i++) {
        uint8_t n = (uint8_t)(root_start + i);
        if ((get_param(n, PARAM_FUNCTION_TYPE) & 0xFFu) == 1u) {
            *audio_fg = n;
            break;
        }
    }
    if (!*audio_fg) {
        g_error = "codec has no Audio Function Group";
        return 0;
    }
    (void)verb12(*audio_fg, 0x705u, 0u, 0);
    delay_ms(2u);

    uint32_t widgets = get_param(*audio_fg, PARAM_NODE_COUNT);
    uint8_t start = (uint8_t)(widgets >> 16);
    uint8_t count = (uint8_t)widgets;
    for (uint32_t i = 0; i < MAX_NODES; i++) g_widget_caps[i] = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t n = (uint8_t)(start + i);
        g_widget_caps[n] = get_param(n, PARAM_AUDIO_WIDGET_CAP);
        (void)verb12(n, 0x705u, 0u, 0);
    }
    delay_ms(2u);

    uint8_t hp_pin = 0;
    g_last_hp_present = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t n = (uint8_t)(start + i);
        if (((g_widget_caps[n] >> 20) & 0x0Fu) != 4u) continue;
        if (!(get_param(n, PARAM_PIN_CAP) & (1u << 4))) continue;
        uint32_t cfg = 0; (void)verb12(n, 0xF1Cu, 0u, &cfg);
        if (((cfg >> 20) & 0x0Fu) == 2u) {
            if (!hp_pin) hp_pin = n;
            if (headphone_present(n)) { hp_pin = n; g_last_hp_present = 1; break; }
        }
    }

    uint8_t priority[3];
    if (g_output_mode == 2 || (g_output_mode == 0 && g_last_hp_present)) {
        priority[0] = 2u; priority[1] = 1u; priority[2] = 0u;
    } else {
        priority[0] = 1u; priority[1] = 0u; priority[2] = 2u;
    }
    *pin = 0; *dac = 0;
    uint8_t selected_device = 0xFFu;
    for (uint8_t pr = 0; pr < 3u && !*pin; pr++) {
        uint8_t wanted = priority[pr];
        for (uint8_t i = 0; i < count && !*pin; i++) {
            uint8_t n = (uint8_t)(start + i);
            if (((g_widget_caps[n] >> 20) & 0x0Fu) != 4u) continue;
            if (!(get_param(n, PARAM_PIN_CAP) & (1u << 4))) continue;
            uint32_t cfg = 0; (void)verb12(n, 0xF1Cu, 0u, &cfg);
            if (((cfg >> 20) & 0x0Fu) != wanted) continue;
            for (uint32_t k = 0; k < MAX_NODES; k++) g_seen[k] = 0;
            uint8_t found_dac = 0;
            if (route_to_dac(n, 0, &found_dac)) {
                *pin = n; *dac = found_dac; selected_device = wanted;
            }
        }
    }
    if (!*dac) {
        for (uint8_t i = 0; i < count; i++) {
            uint8_t n = (uint8_t)(start + i);
            if (((g_widget_caps[n] >> 20) & 0x0Fu) == 0u) { *dac = n; break; }
        }
    }
    if (!*pin) {
        for (uint8_t i = 0; i < count; i++) {
            uint8_t n = (uint8_t)(start + i);
            if (((g_widget_caps[n] >> 20) & 0x0Fu) == 4u &&
                (get_param(n, PARAM_PIN_CAP) & (1u << 4))) {
                *pin = n;
                uint32_t cfg = 0; (void)verb12(n, 0xF1Cu, 0u, &cfg);
                selected_device = (uint8_t)((cfg >> 20) & 0x0Fu);
                break;
            }
        }
    }
    if (!*dac || !*pin) {
        g_error = "no usable speaker pin / DAC path in codec";
        return 0;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t n = (uint8_t)(start + i);
        if (((g_widget_caps[n] >> 20) & 0x0Fu) != 4u || n == *pin) continue;
        uint32_t cfg = 0; (void)verb12(n, 0xF1Cu, 0u, &cfg);
        uint8_t devtype = (uint8_t)((cfg >> 20) & 0x0Fu);
        if (devtype == 0u || devtype == 1u || devtype == 2u)
            (void)verb12(n, 0x707u, 0u, 0);
    }
    uint8_t pinctl = selected_device == 2u ? 0xC0u : 0x40u;
    (void)verb12(*pin, 0x707u, pinctl, 0);
    (void)verb12(*pin, 0x70Cu, 0x02u, 0);
    g_last_output_hp = selected_device == 2u;
    g_last_output_pin = *pin;

    uint16_t gcap = r16(HDA_GCAP);
    uint8_t input_streams = (uint8_t)((gcap >> 8) & 0x0Fu);
    uint8_t output_streams = (uint8_t)((gcap >> 12) & 0x0Fu);
    if (!output_streams) {
        g_error = "HDA controller reports zero output streams";
        return 0;
    }
    *stream = input_streams;
    return 1;
}

int hda_play_pcm16_stereo(const int16_t *pcm, uint32_t bytes,
                          uint32_t sample_rate, hda_info_t *info) {
    g_error = "unknown HDA error";
    if (!pcm || bytes < 4u || (bytes & 3u)) {
        g_error = "PCM buffer is empty or not stereo-aligned";
        return 0;
    }
    uint16_t format = make_format(sample_rate);
    if (format == 0xFFFFu) {
        g_error = "WAV sample rate is not representable by HDA";
        return 0;
    }

    pci_dev_t dev;
    uint8_t afg, pin, dac, stream_index;
    if (!init_controller(&dev, &afg, &pin, &dac, &stream_index)) return 0;

    for (uint32_t n = 1u; n < MAX_NODES; n++) {
        uint32_t caps = output_amp_caps((uint8_t)n, afg);
        if (caps) set_amp_gain((uint8_t)n, caps, 100u);
    }
    uint8_t volume_node = dac;
    uint32_t volume_caps = output_amp_caps(dac, afg);
    if (!volume_caps) {
        volume_node = pin;
        volume_caps = output_amp_caps(pin, afg);
    }
    if (volume_caps) set_amp_gain(volume_node, volume_caps, g_volume_percent);

    uint32_t entries = (bytes + BDL_CHUNK - 1u) / BDL_CHUNK;
    if (!entries || entries > MAX_BDL) {
        g_error = "PCM data is larger than the HDA DMA limit";
        return 0;
    }
    uintptr_t p = (uintptr_t)pcm;
    uint32_t left = bytes;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t chunk = left > BDL_CHUNK ? BDL_CHUNK : left;
        g_bdl[i].addr_lo = (uint32_t)p;
        g_bdl[i].addr_hi = 0;
        g_bdl[i].length = chunk;
        g_bdl[i].flags = (i + 1u == entries) ? 1u : 0u;
        p += chunk; left -= chunk;
    }

    uint32_t sd = HDA_SD_BASE + (uint32_t)stream_index * HDA_SD_SIZE;
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
    (void)wait8(sd + SD_CTL, 2u, 0u, 100u);
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 1u));
    if (!wait8(sd + SD_CTL, 1u, 1u, 100u)) {
        g_error = "HDA output stream reset-set timeout";
        return 0;
    }
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~1u));
    if (!wait8(sd + SD_CTL, 1u, 0u, 100u)) {
        g_error = "HDA output stream reset-clear timeout";
        return 0;
    }

    w32(sd + SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
    w32(sd + SD_BDPU, 0u);
    w32(sd + SD_CBL, bytes);
    w16(sd + SD_LVI, (uint16_t)(entries - 1u));
    w16(sd + SD_FMT, format);
    w8(sd + SD_STS, 0x1Cu);
    w8(sd + SD_CTL + 2u, 0x10u);

    (void)verb12(dac, 0x706u, 0x10u, 0);
    (void)verb4(dac, 0x2u, format, 0);
    wbinvd();
    __asm__ volatile ("" ::: "memory");
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));

    if (info) {
        info->vendor_id = dev.vendor_id; info->device_id = dev.device_id;
        info->codec = g_codec; info->audio_fg = afg;
        info->dac_node = dac; info->pin_node = pin;
        info->stream_index = stream_index; info->sample_rate = sample_rate;
        info->volume_percent = g_volume_percent;
        info->volume_control = volume_caps ? 1u : 0u;
    }

    int result = 1;
    for (;;) {
        uint8_t sts = r8(sd + SD_STS);
        if (sts & 0x04u) break;
        if (sts & 0x18u) {
            g_error = "HDA DMA FIFO/descriptor error";
            result = 0;
            break;
        }
        char key = kbd_poll();
        if (key == 27) { result = 2; break; }
        if (volume_caps && (key == KEY_UP || key == KEY_DOWN)) {
            if (key == KEY_UP && g_volume_percent <= 95u) g_volume_percent += 5u;
            if (key == KEY_DOWN && g_volume_percent >= 5u) g_volume_percent -= 5u;
            set_amp_gain(volume_node, volume_caps, g_volume_percent);
            if (info) info->volume_percent = g_volume_percent;
        }
    }
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
    (void)wait8(sd + SD_CTL, 2u, 0u, 100u);
    w8(sd + SD_STS, 0x1Cu);
    (void)verb12(dac, 0x706u, 0u, 0);
    if (result) g_error = "no error";
    return result;
}

int hda_play_stream_48k(hda_stream_fill fill, hda_stream_seek seek,
                        hda_stream_status status, void *context,
                        hda_info_t *info) {
    g_error = "unknown HDA stream error";
    if (!fill) { g_error = "HDA stream has no fill callback"; return 0; }
    hda_bg_stop();

    pci_dev_t dev;
    uint8_t afg, pin, dac, stream_index;
    if (!init_controller(&dev, &afg, &pin, &dac, &stream_index)) return 0;
    uint16_t format = make_format(48000u);

    for (uint32_t n = 1u; n < MAX_NODES; n++) {
        uint32_t caps = output_amp_caps((uint8_t)n, afg);
        if (caps) set_amp_gain((uint8_t)n, caps, 100u);
    }
    uint8_t volume_node = dac;
    uint32_t volume_caps = output_amp_caps(dac, afg);
    if (!volume_caps) { volume_node = pin; volume_caps = output_amp_caps(pin, afg); }
    if (volume_caps) set_amp_gain(volume_node, volume_caps, g_volume_percent);

    uint64_t generated = 0;
    int eof = 0;
    for (uint32_t i = 0; i < STREAM_BLOCKS; i++) {
        uint32_t got = eof ? 0u : fill(context, (int16_t *)(void *)g_stream_ring[i],
                                      STREAM_BLOCK_BYTES);
        if (got > STREAM_BLOCK_BYTES) got = STREAM_BLOCK_BYTES;
        got &= ~3u;
        generated += got;
        if (got < STREAM_BLOCK_BYTES) {
            memset(g_stream_ring[i] + got, 0, STREAM_BLOCK_BYTES - got);
            eof = 1;
        }
        g_bdl[i].addr_lo = (uint32_t)(uintptr_t)g_stream_ring[i];
        g_bdl[i].addr_hi = 0;
        g_bdl[i].length = STREAM_BLOCK_BYTES;
        g_bdl[i].flags = 1u;
    }
    if (!generated) { g_error = "WAV stream contains no PCM samples"; return 0; }

    uint32_t sd = HDA_SD_BASE + (uint32_t)stream_index * HDA_SD_SIZE;
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
    (void)wait8(sd + SD_CTL, 2u, 0u, 100u);
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 1u));
    if (!wait8(sd + SD_CTL, 1u, 1u, 100u)) {
        g_error = "HDA stream reset-set timeout"; return 0;
    }
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~1u));
    if (!wait8(sd + SD_CTL, 1u, 0u, 100u)) {
        g_error = "HDA stream reset-clear timeout"; return 0;
    }
    w32(sd + SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
    w32(sd + SD_BDPU, 0u);
    w32(sd + SD_CBL, STREAM_BLOCKS * STREAM_BLOCK_BYTES);
    w16(sd + SD_LVI, (uint16_t)(STREAM_BLOCKS - 1u));
    w16(sd + SD_FMT, format);
    w8(sd + SD_STS, 0x1Cu);
    w8(sd + SD_CTL + 2u, 0x10u);
    (void)verb12(dac, 0x706u, 0x10u, 0);
    (void)verb4(dac, 0x2u, format, 0);
    wbinvd();
    __asm__ volatile ("" ::: "memory");
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));

    if (info) {
        info->vendor_id = dev.vendor_id; info->device_id = dev.device_id;
        info->codec = g_codec; info->audio_fg = afg;
        info->dac_node = dac; info->pin_node = pin;
        info->stream_index = stream_index; info->sample_rate = 48000u;
        info->volume_percent = g_volume_percent;
        info->volume_control = volume_caps ? 1u : 0u;
        info->repeat_enabled = (uint8_t)g_repeat_enabled;
        info->shuffle_enabled = (uint8_t)g_shuffle_enabled;
    }

    const uint32_t ring_bytes = STREAM_BLOCKS * STREAM_BLOCK_BYTES;
    uint32_t last_pos = r32(sd + SD_LPIB) % ring_bytes;
    uint64_t consumed = 0;
    uint64_t freed_blocks = 0;
    uint64_t status_next = rdtsc();
    uint64_t status_ticks = tsc_per_ms() * 100u;
    int paused = 0;
    int result = 1;
    for (;;) {
        uint8_t sts = r8(sd + SD_STS);
        if (sts & 0x18u) {
            g_error = "HDA streaming DMA FIFO/descriptor error";
            result = 0; break;
        }
        if (sts & 0x04u) w8(sd + SD_STS, 0x04u);
        uint32_t pos = r32(sd + SD_LPIB) % ring_bytes;
        uint32_t delta = pos >= last_pos ? pos - last_pos
                                        : ring_bytes - last_pos + pos;
        if (!paused) consumed += delta;
        last_pos = pos;

        uint64_t now_freed = consumed / STREAM_BLOCK_BYTES;
        while (freed_blocks < now_freed) {
            uint32_t index = (uint32_t)(freed_blocks % STREAM_BLOCKS);
            if (!eof) {
                uint32_t got = fill(context,
                    (int16_t *)(void *)g_stream_ring[index], STREAM_BLOCK_BYTES);
                if (got > STREAM_BLOCK_BYTES) got = STREAM_BLOCK_BYTES;
                got &= ~3u;
                generated += got;
                if (got < STREAM_BLOCK_BYTES) {
                    memset(g_stream_ring[index] + got, 0,
                           STREAM_BLOCK_BYTES - got);
                    eof = 1;
                }
            }
            freed_blocks++;
        }
        if (eof && consumed >= generated) break;

        uint64_t now = rdtsc();
        if (status && (int64_t)(now - status_next) >= 0) {
            status(context, consumed, paused, g_volume_percent,
                   g_repeat_enabled, g_shuffle_enabled);
            status_next = now + status_ticks;
        }

        char key = kbd_poll();
        if (key == 27) { result = 2; break; }
        if (key == ' ') {
            paused = !paused;
            if (paused) w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
            else        w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));
            last_pos = r32(sd + SD_LPIB) % ring_bytes;
            if (status) status(context, consumed, paused, g_volume_percent,
                               g_repeat_enabled, g_shuffle_enabled);
            continue;
        }
        if (key == 'r' || key == 'R') {
            g_repeat_enabled = !g_repeat_enabled;
            if (info) info->repeat_enabled = (uint8_t)g_repeat_enabled;
            if (status) status(context, consumed, paused, g_volume_percent,
                               g_repeat_enabled, g_shuffle_enabled);
            continue;
        }
        if (key == 's' || key == 'S') {
            g_shuffle_enabled = !g_shuffle_enabled;
            if (info) info->shuffle_enabled = (uint8_t)g_shuffle_enabled;
            if (status) status(context, consumed, paused, g_volume_percent,
                               g_repeat_enabled, g_shuffle_enabled);
            continue;
        }
        if (seek && (key == KEY_LEFT || key == KEY_RIGHT)) {
            int32_t seconds = key == KEY_LEFT ? -10 : 10;
            w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
            if (seek(context, seconds, consumed)) {
                generated = 0; eof = 0;
                for (uint32_t i = 0; i < STREAM_BLOCKS; i++) {
                    uint32_t got = eof ? 0u : fill(context,
                        (int16_t *)(void *)g_stream_ring[i], STREAM_BLOCK_BYTES);
                    if (got > STREAM_BLOCK_BYTES) got = STREAM_BLOCK_BYTES;
                    got &= ~3u; generated += got;
                    if (got < STREAM_BLOCK_BYTES) {
                        memset(g_stream_ring[i] + got, 0, STREAM_BLOCK_BYTES - got);
                        eof = 1;
                    }
                }
                w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 1u));
                (void)wait8(sd + SD_CTL, 1u, 1u, 100u);
                w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~1u));
                (void)wait8(sd + SD_CTL, 1u, 0u, 100u);
                w32(sd + SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
                w32(sd + SD_BDPU, 0u);
                w32(sd + SD_CBL, ring_bytes);
                w16(sd + SD_LVI, (uint16_t)(STREAM_BLOCKS - 1u));
                w16(sd + SD_FMT, format);
                w8(sd + SD_STS, 0x1Cu);
                w8(sd + SD_CTL + 2u, 0x10u);
                (void)verb12(dac, 0x706u, 0x10u, 0);
                (void)verb4(dac, 0x2u, format, 0);
                wbinvd();
                consumed = 0; freed_blocks = 0;
                last_pos = 0; paused = 0;
                w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));
                if (status) status(context, consumed, paused, g_volume_percent,
                                   g_repeat_enabled, g_shuffle_enabled);
            } else if (!paused) {
                w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));
            }
            continue;
        }
        if (volume_caps && (key == KEY_UP || key == KEY_DOWN)) {
            if (key == KEY_UP && g_volume_percent <= 95u) g_volume_percent += 5u;
            if (key == KEY_DOWN && g_volume_percent >= 5u) g_volume_percent -= 5u;
            set_amp_gain(volume_node, volume_caps, g_volume_percent);
            if (info) info->volume_percent = g_volume_percent;
            if (status) status(context, consumed, paused, g_volume_percent,
                               g_repeat_enabled, g_shuffle_enabled);
        }
    }
    if (status) status(context, consumed, paused, g_volume_percent,
                       g_repeat_enabled, g_shuffle_enabled);

    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
    (void)wait8(sd + SD_CTL, 2u, 0u, 100u);
    w8(sd + SD_STS, 0x1Cu);
    (void)verb12(dac, 0x706u, 0u, 0);
    if (result) g_error = "no error";
    return result;
}

typedef struct {
    int active, paused, eof;
    uint32_t sd, ring_bytes, last_pos;
    uint8_t dac, volume_node;
    uint32_t volume_caps;
    uint64_t generated, consumed, freed_blocks;
    hda_stream_fill fill;
    hda_stream_seek seek;
    hda_bg_done done;
    void *context;
} hda_bg_state;

static hda_bg_state g_bg;

void hda_bg_stop(void) {
    if (!g_bg.active) return;
    w8(g_bg.sd + SD_CTL, (uint8_t)(r8(g_bg.sd + SD_CTL) & ~2u));
    (void)wait8(g_bg.sd + SD_CTL, 2u, 0u, 100u);
    w8(g_bg.sd + SD_STS, 0x1Cu);
    (void)verb12(g_bg.dac, 0x706u, 0u, 0);
    g_bg.active = 0; g_bg.paused = 0;
}

int hda_bg_start_48k(hda_stream_fill fill, hda_stream_seek seek,
                     hda_bg_done done, void *context, hda_info_t *info) {
    if (!fill) { g_error = "background stream has no fill callback"; return 0; }
    hda_bg_stop();
    memset(&g_bg, 0, sizeof g_bg);

    pci_dev_t dev;
    uint8_t afg, pin, dac, stream_index;
    if (!init_controller(&dev, &afg, &pin, &dac, &stream_index)) return 0;
    uint16_t format = make_format(48000u);
    for (uint32_t n = 1u; n < MAX_NODES; n++) {
        uint32_t caps = output_amp_caps((uint8_t)n, afg);
        if (caps) set_amp_gain((uint8_t)n, caps, 100u);
    }
    uint8_t volume_node = dac;
    uint32_t volume_caps = output_amp_caps(dac, afg);
    if (!volume_caps) { volume_node = pin; volume_caps = output_amp_caps(pin, afg); }
    if (volume_caps) set_amp_gain(volume_node, volume_caps, g_volume_percent);

    for (uint32_t i = 0; i < STREAM_BLOCKS; i++) {
        uint32_t got = g_bg.eof ? 0u : fill(context,
            (int16_t *)(void *)g_stream_ring[i], STREAM_BLOCK_BYTES);
        if (got > STREAM_BLOCK_BYTES) got = STREAM_BLOCK_BYTES;
        got &= ~3u; g_bg.generated += got;
        if (got < STREAM_BLOCK_BYTES) {
            memset(g_stream_ring[i] + got, 0, STREAM_BLOCK_BYTES - got);
            g_bg.eof = 1;
        }
        g_bdl[i].addr_lo = (uint32_t)(uintptr_t)g_stream_ring[i];
        g_bdl[i].addr_hi = 0; g_bdl[i].length = STREAM_BLOCK_BYTES;
        g_bdl[i].flags = 1u;
    }
    if (!g_bg.generated) { g_error = "background WAV contains no PCM"; return 0; }

    uint32_t sd = HDA_SD_BASE + (uint32_t)stream_index * HDA_SD_SIZE;
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~2u));
    (void)wait8(sd + SD_CTL, 2u, 0u, 100u);
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 1u));
    if (!wait8(sd + SD_CTL, 1u, 1u, 100u)) {
        g_error = "background HDA reset-set timeout"; return 0;
    }
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~1u));
    if (!wait8(sd + SD_CTL, 1u, 0u, 100u)) {
        g_error = "background HDA reset-clear timeout"; return 0;
    }
    g_bg.ring_bytes = STREAM_BLOCKS * STREAM_BLOCK_BYTES;
    w32(sd + SD_BDPL, (uint32_t)(uintptr_t)g_bdl); w32(sd + SD_BDPU, 0u);
    w32(sd + SD_CBL, g_bg.ring_bytes);
    w16(sd + SD_LVI, (uint16_t)(STREAM_BLOCKS - 1u));
    w16(sd + SD_FMT, format); w8(sd + SD_STS, 0x1Cu);
    w8(sd + SD_CTL + 2u, 0x10u);
    (void)verb12(dac, 0x706u, 0x10u, 0);
    (void)verb4(dac, 0x2u, format, 0);
    wbinvd();

    g_bg.sd = sd; g_bg.dac = dac; g_bg.volume_node = volume_node;
    g_bg.volume_caps = volume_caps; g_bg.fill = fill; g_bg.seek = seek;
    g_bg.done = done; g_bg.context = context; g_bg.last_pos = 0; g_bg.active = 1;
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | 2u));

    if (info) {
        info->vendor_id = dev.vendor_id; info->device_id = dev.device_id;
        info->codec = g_codec; info->audio_fg = afg; info->dac_node = dac;
        info->pin_node = pin; info->stream_index = stream_index;
        info->sample_rate = 48000u; info->volume_percent = g_volume_percent;
        info->volume_control = volume_caps ? 1u : 0u;
        info->repeat_enabled = 0; info->shuffle_enabled = 0;
    }
    g_error = "no error";
    return 1;
}

void hda_bg_poll(void) {
    if (!g_bg.active || g_bg.paused) return;
    uint8_t sts = r8(g_bg.sd + SD_STS);
    if (sts & 0x18u) { g_error = "background HDA DMA underrun"; hda_bg_stop(); return; }
    if (sts & 0x04u) w8(g_bg.sd + SD_STS, 0x04u);
    uint32_t pos = r32(g_bg.sd + SD_LPIB) % g_bg.ring_bytes;
    uint32_t delta = pos >= g_bg.last_pos ? pos - g_bg.last_pos
                                         : g_bg.ring_bytes - g_bg.last_pos + pos;
    g_bg.consumed += delta; g_bg.last_pos = pos;
    uint64_t now_freed = g_bg.consumed / STREAM_BLOCK_BYTES;
    while (g_bg.freed_blocks < now_freed) {
        uint32_t index = (uint32_t)(g_bg.freed_blocks % STREAM_BLOCKS);
        if (!g_bg.eof) {
            uint32_t got = g_bg.fill(g_bg.context,
                (int16_t *)(void *)g_stream_ring[index], STREAM_BLOCK_BYTES);
            if (got > STREAM_BLOCK_BYTES) got = STREAM_BLOCK_BYTES;
            got &= ~3u; g_bg.generated += got;
            if (got < STREAM_BLOCK_BYTES) {
                memset(g_stream_ring[index] + got, 0, STREAM_BLOCK_BYTES - got);
                g_bg.eof = 1;
            }
        }
        g_bg.freed_blocks++;
    }
    if (g_bg.eof && g_bg.consumed >= g_bg.generated) {
        hda_bg_done done = g_bg.done;
        void *context = g_bg.context;
        hda_bg_stop();
        if (done) done(context);
    }
}

void hda_bg_set_paused(int paused) {
    if (!g_bg.active) return;
    paused = paused ? 1 : 0;
    if (paused == g_bg.paused) return;
    g_bg.paused = paused;
    if (paused) w8(g_bg.sd + SD_CTL, (uint8_t)(r8(g_bg.sd + SD_CTL) & ~2u));
    else {
        g_bg.last_pos = r32(g_bg.sd + SD_LPIB) % g_bg.ring_bytes;
        w8(g_bg.sd + SD_CTL, (uint8_t)(r8(g_bg.sd + SD_CTL) | 2u));
    }
}

int hda_bg_is_active(void) { return g_bg.active; }
int hda_bg_is_paused(void) { return g_bg.paused; }
uint64_t hda_bg_played_bytes(void) { return g_bg.consumed; }
uint8_t hda_bg_get_volume(void) { return g_volume_percent; }

void hda_bg_set_volume(uint8_t percent) {
    if (percent > 100u) percent = 100u;
    g_volume_percent = percent;
    if (g_bg.active && g_bg.volume_caps)
        set_amp_gain(g_bg.volume_node, g_bg.volume_caps, g_volume_percent);
}

void hda_set_output_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_output_mode = mode;
}
int hda_get_output_mode(void) { return g_output_mode; }
int hda_headphones_present(void) { return g_last_hp_present; }
int hda_output_is_headphones(void) { return g_last_output_hp; }
uint8_t hda_output_pin(void) { return g_last_output_pin; }

const char *hda_last_error(void) { return g_error; }
