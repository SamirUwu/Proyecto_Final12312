#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <NIDAQmx.h>
#include "../include/distortion.h"

#define POT_MAX_STEPS 99

static int        potStep      = 0;
static int        chipSelected = 0;
static TaskHandle doTask       = 0;
static int        hw_ok        = 0;
static char       ni_device_name[64] = "Dev1";  // fallback

// ── Auto-detectar nombre del dispositivo NI ───────────────────────────────────
static void detect_ni_device(void)
{
    char buf[1024] = {0};
    int32 e = DAQmxGetSystemInfoAttribute(DAQmx_Sys_DevNames, buf, sizeof(buf));
    if (e < 0 || buf[0] == '\0') {
        printf("[DIST] No se pudo detectar dispositivo NI, usando %s\n", ni_device_name);
        return;
    }

    // Tomar el primer dispositivo de la lista (separada por comas)
    char *comma = strchr(buf, ',');
    if (comma) *comma = '\0';

    // Trim espacios
    char *start = buf;
    while (*start == ' ') start++;
    char *end = start + strlen(start) - 1;
    while (end > start && *end == ' ') *end-- = '\0';

    strncpy(ni_device_name, start, sizeof(ni_device_name) - 1);
    ni_device_name[sizeof(ni_device_name) - 1] = '\0';
    printf("[DIST] Dispositivo NI detectado: %s\n", ni_device_name);
}

// ── Raw line writer ───────────────────────────────────────────────────────────
static int WriteLines(uInt8 inc, uInt8 ud, uInt8 cs)
{
    if (!hw_ok) return 0;

    uInt8 data[3] = { inc, ud, cs };
    int32 e = DAQmxWriteDigitalLines(doTask, 1, 1, 10.0,
                                     DAQmx_Val_GroupByChannel,
                                     data, NULL, NULL);
    if (e < 0) {
        char eb[256];
        DAQmxGetExtendedErrorInfo(eb, sizeof(eb));
        fprintf(stderr, "[DIST ERR] WriteDigital: %d  %s\n", (int)e, eb);
        hw_ok = 0;
        return 0;
    }
    return 1;
}

// ── Assert CS LOW ─────────────────────────────────────────────────────────────
static void SelectChip(void)
{
    WriteLines(1, 1, 0);
    chipSelected = 1;
}

// ── Move wiper one tap ────────────────────────────────────────────────────────
static void StepWiper(int direction)
{
    if (!hw_ok) return;
    if (!chipSelected) SelectChip();

    uInt8 ud = (direction > 0) ? 1 : 0;

    WriteLines(1, ud, 0);
    Sleep(1);
    WriteLines(0, ud, 0);
    Sleep(1);
    WriteLines(1, ud, 0);
    Sleep(1);

    potStep += direction;
    if (potStep < 0)             potStep = 0;
    if (potStep > POT_MAX_STEPS) potStep = POT_MAX_STEPS;
}

// ── Save wiper to NVM ─────────────────────────────────────────────────────────
static void StoreAndDeselect(void)
{
    if (!hw_ok) return;
    if (!chipSelected) return;

    WriteLines(1, 1, 0);
    Sleep(1);
    WriteLines(1, 1, 1);
    Sleep(20);

    chipSelected = 0;
    printf("[DIST] Stored to NVM. Step=%d\n", potStep);
}

// ── Public API ────────────────────────────────────────────────────────────────

void distortion_init(void)
{
    // Auto-detectar dispositivo NI antes de crear la tarea
    detect_ni_device();

    int32 e;
    hw_ok = 0;

    e = DAQmxCreateTask("distDoTask", &doTask);
    if (e < 0) {
        fprintf(stderr, "[DIST] NI no disponible — digipot deshabilitado\n");
        return;
    }

    // Construir string del canal con el dispositivo detectado
    char chan_str[128];
    snprintf(chan_str, sizeof(chan_str), "%s/port0/line0:2", ni_device_name);
    printf("[DIST] Usando canal: %s\n", chan_str);

    e = DAQmxCreateDOChan(doTask, chan_str, "", DAQmx_Val_ChanPerLine);
    if (e < 0) {
        fprintf(stderr, "[DIST] CreateDOChan fallo — digipot deshabilitado\n");
        DAQmxClearTask(doTask);
        doTask = 0;
        return;
    }

    e = DAQmxStartTask(doTask);
    if (e < 0) {
        fprintf(stderr, "[DIST] StartTask fallo — digipot deshabilitado\n");
        DAQmxClearTask(doTask);
        doTask = 0;
        return;
    }

    // Escritura de prueba con timeout corto
    uInt8 probe[3] = { 1, 1, 1 };
    int32 probe_err = DAQmxWriteDigitalLines(doTask, 1, 1, 2.0,
                                             DAQmx_Val_GroupByChannel,
                                             probe, NULL, NULL);
    if (probe_err < 0) {
        fprintf(stderr, "[DIST] NI USB-6009 no conectada — digipot deshabilitado\n");
        DAQmxStopTask(doTask);
        DAQmxClearTask(doTask);
        doTask = 0;
        hw_ok  = 0;
        return;
    }

    hw_ok = 1;
    printf("[DIST] Init OK — NI USB-6009 conectada (%s)\n", ni_device_name);

    SelectChip();
    while (potStep > 0)
        StepWiper(-1);

    printf("[DIST] Wiper reset to position 0\n");
}

typedef struct { int target; } WiperArgs;

static DWORD WINAPI wiper_thread(LPVOID arg)
{
    WiperArgs *a = (WiperArgs *)arg;
    int target = a->target;
    free(a);

    int delta = target - potStep;
    if (delta == 0) return 0;

    int dir   = (delta > 0) ? +1 : -1;
    int steps = abs(delta);

    SelectChip();
    for (int i = 0; i < steps; i++)
        StepWiper(dir);

    printf("[DIST] Volume -> wiper %d\n", potStep);
    return 0;
}

void distortion_set_volume(float volume)
{
    if (!hw_ok) {
        potStep = (int)(volume * POT_MAX_STEPS);
        printf("[DIST] (sin hardware) volume=%.2f -> step logico=%d\n", volume, potStep);
        return;
    }

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    WiperArgs *args = malloc(sizeof(WiperArgs));
    args->target = (int)(volume * POT_MAX_STEPS);

    HANDLE t = CreateThread(NULL, 0, wiper_thread, args, 0, NULL);
    if (t) CloseHandle(t);
}

void distortion_store(void)
{
    if (hw_ok)
        StoreAndDeselect();

    if (doTask) {
        DAQmxStopTask(doTask);
        DAQmxClearTask(doTask);
        doTask = 0;
    }
    printf("[DIST] Shutdown complete\n");
}