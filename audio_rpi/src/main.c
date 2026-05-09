#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>
#include <portaudio.h>
#include <pa_win_wasapi.h>

#include "../include/socket_server.h"
#include "../include/serial_input.h"
#include "../include/delay.h"
#include "../include/overdrive.h"
#include "../include/wah.h"
#include "../include/chorus.h"
#include "../include/flanger.h"
#include "../include/pitch_shifter.h"
#include "../include/phaser.h"
#include "../include/reverb.h"
#include "../include/distortion.h"
#include "../include/config.h"

#define PI                 3.14159265358979323846f
#define ALSA_WRITE_BATCHES 1
#define ALSA_PERIOD_FRAMES (SERIAL_PACKET_SAMPLES * ALSA_WRITE_BATCHES)

#define SIM_MODE    1
#define SERIAL_PORT NULL
#define SERIAL_BAUD 460800

// ── PortAudio (replaces ALSA) ─────────────────────────────────────────────────

static PaStream *alsa_init(unsigned int sample_rate)
{
    Pa_Initialize();
    PaStreamParameters out;
    memset(&out, 0, sizeof(out));
    out.device                    = Pa_GetDefaultOutputDevice();
    out.channelCount              = 1;
    out.sampleFormat              = paInt16;
    out.suggestedLatency          = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = NULL;

    PaStream *stream;
    PaError err = Pa_OpenStream(&stream, NULL, &out,
                                sample_rate, ALSA_PERIOD_FRAMES,
                                paClipOff, NULL, NULL);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return NULL;
    }
    Pa_StartStream(stream);
    printf("PortAudio ready at %u Hz | period=%d frames\n",
           sample_rate, ALSA_PERIOD_FRAMES);
    return stream;
}

static void alsa_write_safe(PaStream *stream, const int16_t *buf, int frames)
{
    PaError err = Pa_WriteStream(stream, buf, frames);
    if (err == paOutputUnderflowed)
        fprintf(stderr, "[pa] underflow\n");
    else if (err != paNoError)
        fprintf(stderr, "[pa] write error: %s\n", Pa_GetErrorText(err));
}

// ── Effect IDs ────────────────────────────────────────────────────────────────

#define FX_OVERDRIVE    0
#define FX_WAH          1
#define FX_DELAY        2
#define FX_CHORUS       3
#define FX_FLANGER      4
#define FX_PITCHSHIFTER 5
#define FX_PHASER       6
#define FX_REVERB       7
#define FX_DISTORTION   8
#define FX_COUNT        9  

int enabled[FX_COUNT]  = {0};
int fx_order[FX_COUNT] = {0};
int fx_order_count     = 0;

float master_gain = MASTER_GAIN_DEFAULT;

char json_buffer[4096];

typedef struct {
    const char *effect_key;
    const char *param_key;
    int         fx_id;
    float      *target;
    float       scale;
    float       offset;
} ParamMap;

float process_effect(int fx_id, float sig,
                     Overdrive *od, Wah *wah, Chorus *ch,
                     Flanger *flanger, PitchShifter *pitch,
                     Delay *delay, Phaser *phaser, Reverb *reverb)
{
    switch (fx_id) {
        case FX_OVERDRIVE:    return Overdrive_process(od, sig);
        case FX_DISTORTION: return sig;
        case FX_WAH:          return Wah_process(wah, sig);
        case FX_CHORUS:       return Chorus_process(ch, sig);
        case FX_FLANGER:      return Flanger_process(flanger, sig);
        case FX_PITCHSHIFTER: return PitchShifter_process(pitch, sig);
        case FX_DELAY:        return Delay_process(delay, sig);
        case FX_PHASER:       return Phaser_process(phaser, sig);
        case FX_REVERB:       return Reverb_process(reverb, sig);
        default:              return sig;
    }
}

// ── Windows named pipe packet reader ─────────────────────────────────────────

#if SIM_MODE == 3
static int win_read_packet(HANDLE h, uint16_t *out_samples)
{
    static const uint8_t SYNC[4] = {0xAA, 0x55, 0xFF, 0x00};
    uint8_t window[4] = {0};
    DWORD read_bytes;

    for (int t = 0; t < SERIAL_PACKET_SAMPLES * 16; t++) {
        uint8_t b;
        if (!ReadFile(h, &b, 1, &read_bytes, NULL) || read_bytes == 0)
            return -1;
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = b;
        if (memcmp(window, SYNC, 4) == 0)
            goto found_sync;
    }
    return -1;

found_sync:;
    uint8_t raw[SERIAL_PACKET_SAMPLES * 2];
    DWORD total = 0;
    while (total < sizeof(raw)) {
        if (!ReadFile(h, raw + total, (DWORD)(sizeof(raw) - total), &read_bytes, NULL))
            return -1;
        total += read_bytes;
    }
    for (int i = 0; i < SERIAL_PACKET_SAMPLES; i++)
        out_samples[i] = (uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8);
    return 0;
}
#endif

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    Delay        delay;   Delay_init(&delay, 1.0f, 0.5f, 0.4f);
    Overdrive    od;      Overdrive_init(&od, 0.0f, 0.0f, 0.0f);
    Wah          wah;     Wah_init(&wah, 0.7f, 5.0f, 1.0f);
    Chorus       ch;      Chorus_init(&ch, 0.5f, 0.3f, 0.1f, 0.5f);
    Flanger      flanger; Flanger_init(&flanger, 0.25f, 0.7f, 0.3f, 0.5f);
    PitchShifter pitch;   PitchShifter_init(&pitch, 7.0f, 0.5f);
    Phaser       phaser;  Phaser_init(&phaser, 0.5f, 0.7f, 0.3f, 0.5f);
    Reverb       reverb;  Reverb_init(&reverb, 0.8f, 8000.0f, 0.3f);
    distortion_init();


    ParamMap map[] = {
        { "Overdrive",    "GAIN",        FX_OVERDRIVE,    &od.gain,           1.0f, 0.0f },
        { "Overdrive",    "TONE",        FX_OVERDRIVE,    &od.tone,           1.0f, 0.0f },
        { "Overdrive",    "OUTPUT",      FX_OVERDRIVE,    &od.output,         1.0f, 0.0f },
        { "Wah",          "FREQ",        FX_WAH,          &wah.freq,          1.0f, 0.0f },
        { "Wah",          "Q",           FX_WAH,          &wah.q,             1.0f, 0.0f },
        { "Wah",          "LEVEL",       FX_WAH,          &wah.level,         1.0f, 0.0f },
        { "Delay",        "TIME",        FX_DELAY,        &delay.delay_ms,    1.0f, 0.0f },
        { "Delay",        "FEEDBACK",    FX_DELAY,        &delay.feedback,    1.0f, 0.0f },
        { "Delay",        "MIX",         FX_DELAY,        &delay.mix,         1.0f, 0.0f },
        { "Chorus",       "RATE",        FX_CHORUS,       &ch.rate,           1.0f, 0.0f },
        { "Chorus",       "DEPTH",       FX_CHORUS,       &ch.depth,          1.0f, 0.0f },
        { "Chorus",       "FEEDBACK",    FX_CHORUS,       &ch.feedback,       1.0f, 0.0f },
        { "Chorus",       "MIX",         FX_CHORUS,       &ch.mix,            1.0f, 0.0f },
        { "Flanger",      "RATE",        FX_FLANGER,      &flanger.rate,      1.0f, 0.0f },
        { "Flanger",      "DEPTH",       FX_FLANGER,      &flanger.depth,     1.0f, 0.0f },
        { "Flanger",      "FEEDBACK",    FX_FLANGER,      &flanger.feedback,  1.0f, 0.0f },
        { "Flanger",      "MIX",         FX_FLANGER,      &flanger.mix,       1.0f, 0.0f },
        { "PitchShifter", "SEMITONES",   FX_PITCHSHIFTER, &pitch.semitones_a, 1.0f, 0.0f },
        { "PitchShifter", "SEMITONES_B", FX_PITCHSHIFTER, &pitch.semitones_b, 1.0f, 0.0f },
        { "PitchShifter", "MIX_A",       FX_PITCHSHIFTER, &pitch.mix_a,       1.0f, 0.0f },
        { "PitchShifter", "MIX_B",       FX_PITCHSHIFTER, &pitch.mix_b,       1.0f, 0.0f },
        { "PitchShifter", "MIX",         FX_PITCHSHIFTER, &pitch.mix,         1.0f, 0.0f },
        { "Phaser",       "RATE",        FX_PHASER,       &phaser.rate,       1.0f, 0.0f },
        { "Phaser",       "DEPTH",       FX_PHASER,       &phaser.depth,      1.0f, 0.0f },
        { "Phaser",       "FEEDBACK",    FX_PHASER,       &phaser.feedback,   1.0f, 0.0f },
        { "Phaser",       "MIX",         FX_PHASER,       &phaser.mix,        1.0f, 0.0f },
        { "Reverb",       "FEEDBACK",    FX_REVERB,       &reverb.feedback,   1.0f, 0.0f },
        { "Reverb",       "LPFREQ",      FX_REVERB,       &reverb.lpfreq,     1.0f, 0.0f },
        { "Reverb",       "MIX",         FX_REVERB,       &reverb.mix,        1.0f, 0.0f },
        { "Distortion",   "OUTPUT",      FX_DISTORTION,   NULL              , 0.0f, 0.0f },
    };
    int map_size = sizeof(map) / sizeof(map[0]);

    // ── Input source init ─────────────────────────────────────────────────────
#if SIM_MODE == 0
    uint16_t packet[SERIAL_PACKET_SAMPLES];
    int serial_fd = serial_open(SERIAL_PORT, SERIAL_BAUD);
    if (serial_fd < 0) {
        fprintf(stderr, "No se pudo abrir el serial.\n");
        return 1;
    }
    printf("[SIM_MODE 0] Leyendo desde ESP32 por serial\n");

#elif SIM_MODE == 1
    int sim_i = 0;
    printf("[SIM_MODE 1] Usando suma de senos a 440 Hz\n");

#elif SIM_MODE == 3
    printf("[SIM_MODE 3] Attempting to open pipe...\n");
    HANDLE pipe_handle = CreateFileA(
        "\\\\.\\pipe\\ni6009", GENERIC_READ, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (pipe_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, "[SIM_MODE 3] ERROR: Could not open pipe. Error code: %lu\n", err);
        fprintf(stderr, "  Check that:\n");
        fprintf(stderr, "  1. NI feeder (Python) is running\n");
        fprintf(stderr, "  2. The pipe '\\\\.\\\pipe\\ni6009' exists\n");
        fprintf(stderr, "  3. Python feeder has successfully connected\n");
        return 1;
    }
    printf("[SIM_MODE 3] Pipe opened successfully — streaming from NI feeder\n");
    uint16_t packet3[SERIAL_PACKET_SAMPLES];
#endif

    // ── Audio + socket init ───────────────────────────────────────────────────
    PaStream *pcm = alsa_init(SAMPLE_RATE);
    if (!pcm) return 1;
    socket_init();

    float   batch_pre[SERIAL_PACKET_SAMPLES];
    float   batch_post[SERIAL_PACKET_SAMPLES];
    long    total_samples  = 0;
    int16_t alsa_accum[ALSA_PERIOD_FRAMES];
    int     alsa_accum_pos = 0;

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (1) {

        // Receive JSON from Python GUI
        int n = socket_receive(json_buffer, sizeof(json_buffer) - 1);
        if (n > 0) {
            printf("JSON recibido:\n%s\n", json_buffer);

            memset(enabled,  0, sizeof(enabled));
            memset(fx_order, 0, sizeof(fx_order));
            fx_order_count = 0;

            // Parse master_gain from JSON
            char *mg_pos = strstr(json_buffer, "\"master_gain\"");
            if (mg_pos) {
                char *mg_colon = strchr(mg_pos, ':');
                if (mg_colon) {
                    float mg_val = 0.0f;
                    if (sscanf(mg_colon + 1, "%f", &mg_val) == 1) {
                        if (mg_val < MASTER_GAIN_MIN) mg_val = MASTER_GAIN_MIN;
                        if (mg_val > MASTER_GAIN_MAX) mg_val = MASTER_GAIN_MAX;
                        master_gain = mg_val;
                        printf("  master_gain = %f\n", master_gain);
                    }
                }
            }

            char *cursor = json_buffer;
            while ((cursor = strstr(cursor, "\"type\"")) != NULL) {
                char *type_colon = strchr(cursor, ':');
                if (!type_colon) break;
                char *type_start = strchr(type_colon + 1, '"');
                if (!type_start) break;
                type_start++;
                char *type_end = strchr(type_start, '"');
                if (!type_end) break;

                char effect_type[64] = {0};
                int type_len = (int)(type_end - type_start);
                if (type_len >= (int)sizeof(effect_type))
                    type_len = (int)sizeof(effect_type) - 1;
                strncpy(effect_type, type_start, type_len);
                cursor = type_end + 1;

                int fx_enabled = 1;
                char *enabled_pos = strstr(cursor, "\"enabled\"");
                if (enabled_pos) {
                    char *en_colon = strchr(enabled_pos, ':');
                    if (en_colon) {
                        char *en_val = en_colon + 1;
                        while (*en_val == ' ') en_val++;
                        fx_enabled = (strncmp(en_val, "true", 4) == 0) ? 1 : 0;
                    }
                }

                char *params_pos   = strstr(cursor, "\"params\"");
                if (!params_pos) continue;
                char *params_open  = strchr(params_pos, '{');
                if (!params_open)  continue;
                char *params_close = strchr(params_open, '}');
                if (!params_close) continue;

                char params_block[512] = {0};
                int block_len = (int)(params_close - params_open + 1);
                if (block_len >= (int)sizeof(params_block))
                    block_len = (int)sizeof(params_block) - 1;
                strncpy(params_block, params_open, block_len);

                printf("  [%s] enabled=%d\n", effect_type, fx_enabled);

                for (int m = 0; m < map_size; m++) {
                    if (strcmp(map[m].effect_key, effect_type) != 0) continue;
                    if (enabled[map[m].fx_id] == 0 && fx_order_count < FX_COUNT) {
                        if (fx_enabled)
                            fx_order[fx_order_count++] = map[m].fx_id;
                        enabled[map[m].fx_id] = fx_enabled;
                    }
                    if (!fx_enabled) continue;

                    char search_key[64];
                    snprintf(search_key, sizeof(search_key), "\"%s\"", map[m].param_key);
                    char *param_pos = strstr(params_block, search_key);
                    if (!param_pos) continue;
                    char *colon = strchr(param_pos, ':');
                    if (!colon) continue;
                    float raw_value = 0.0f;
                    if (sscanf(colon + 1, "%f", &raw_value) == 1) {
                        
                        // ── Distortion: hardware call instead of float write ──
                        if (map[m].target == NULL) {
                            if (strcmp(map[m].param_key, "OUTPUT") == 0) {
                                distortion_set_volume(raw_value);
                                printf("    OUTPUT (digipot) = %f\n", raw_value);
                            }
                            continue; // skip the *target write below
                        }

                        *map[m].target = raw_value * map[m].scale + map[m].offset;
                        printf("    %s = %f\n", map[m].param_key, *map[m].target);
                    }
                }
            }
            printf("  Cadena (%d): ", fx_order_count);
            for (int k = 0; k < fx_order_count; k++) printf("%d ", fx_order[k]);
            printf("\n");
            memset(json_buffer, 0, sizeof(json_buffer));
        }

        // ── Read batch of samples ─────────────────────────────────────────────
#if SIM_MODE == 0
        if (serial_read_packet(serial_fd, packet) < 0) {
            fprintf(stderr, "[serial] error leyendo paquete, reintentando...\n");
            continue;
        }
        for (int s = 0; s < SERIAL_PACKET_SAMPLES; s++)
            batch_pre[s] = serial_adc_to_float(packet[s]);

#elif SIM_MODE == 1
        for (int s = 0; s < SERIAL_PACKET_SAMPLES; s++) {
            float t = (float)sim_i / SAMPLE_RATE;
            batch_pre[s] =  0.50f * sinf(2.0f * PI * 220.0f * t)
                          + 0.25f * sinf(2.0f * PI * 440.0f * t)
                          + 0.15f * sinf(2.0f * PI * 660.0f * t)
                          + 0.10f * sinf(2.0f * PI * 880.0f * t);
            sim_i++;
            if (sim_i >= SAMPLE_RATE) sim_i = 0;
        }

#elif SIM_MODE == 3
        if (win_read_packet(pipe_handle, packet3) < 0) {
            fprintf(stderr, "[SIM_MODE 3] pipe read error or feeder closed\n");
            break;
        }
        for (int s = 0; s < SERIAL_PACKET_SAMPLES; s++)
            batch_pre[s] = serial_adc_to_float(packet3[s]);
#endif

        // ── Effect chain ──────────────────────────────────────────────────────
        for (int s = 0; s < SERIAL_PACKET_SAMPLES; s++) {
            float sig = batch_pre[s];
            for (int k = 0; k < fx_order_count; k++)
                sig = process_effect(fx_order[k], sig,
                                     &od, &wah, &ch, &flanger,
                                     &pitch, &delay, &phaser, &reverb);
            batch_post[s] = sig;
            total_samples++;
        }

        // ── Write to audio output ─────────────────────────────────────────────
        for (int s = 0; s < SERIAL_PACKET_SAMPLES; s++) {
            float boosted = batch_post[s] * master_gain;
            // Apply soft-clipping: tanh when boosting, hard-clamp otherwise
            if (master_gain > 1.0f) {
                // Always apply tanh for smooth, continuous soft-clipping
                boosted = tanhf(boosted);
            } else {
                if (boosted >  1.0f) boosted =  1.0f;
                if (boosted < -1.0f) boosted = -1.0f;
            }
            alsa_accum[alsa_accum_pos++] = (int16_t)(boosted * 32767.0f);
        }
        if (alsa_accum_pos >= ALSA_PERIOD_FRAMES) {
            alsa_write_safe(pcm, alsa_accum, ALSA_PERIOD_FRAMES);
            alsa_accum_pos = 0;
        }

        // ── Debug print every ~0.5s ───────────────────────────────────────────
        if (total_samples % 22039 < SERIAL_PACKET_SAMPLES)
            printf("audio: input=%f out=%f  cadena=%d efectos\n",
                   batch_pre[0], batch_post[0], fx_order_count);

        // ── Send to Python visualizer ─────────────────────────────────────────
        socket_send_batch(batch_pre, batch_post, SERIAL_PACKET_SAMPLES);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
#if SIM_MODE == 0
    serial_close(serial_fd);
#elif SIM_MODE == 3
    CloseHandle(pipe_handle);
#endif
    Pa_StopStream(pcm);
    Pa_CloseStream(pcm);
    Pa_Terminate();
    socket_close();
    distortion_store();
    return 0;
}