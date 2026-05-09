#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <NIDAQmx.h>
#include "../include/distortion.h"

#define POT_MAX_STEPS 99

static int        potStep      = 0;
static int        chipSelected = 0;
static TaskHandle doTask       = 0;
static int        hw_ok        = 0;  // 1 solo si la NI está conectada y la tarea OK

// ── Raw line writer ───────────────────────────────────────────────────────────
// line0 = INC, line1 = U/D, line2 = CS
static int WriteLines(uInt8 inc, uInt8 ud, uInt8 cs)
{
    if (!hw_ok) return 0;  // salida rápida sin spam de errores si no hay hardware

    uInt8 data[3] = { inc, ud, cs };
    int32 e = DAQmxWriteDigitalLines(doTask, 1, 1, 10.0,
                                     DAQmx_Val_GroupByChannel,
                                     data, NULL, NULL);
    if (e < 0) {
        char eb[256];
        DAQmxGetExtendedErrorInfo(eb, sizeof(eb));
        fprintf(stderr, "[DIST ERR] WriteDigital: %d  %s\n", (int)e, eb);
        hw_ok = 0;  // marcar como roto para no seguir intentando
        return 0;
    }
    return 1;
}

// ── Assert CS LOW so device starts listening ──────────────────────────────────
static void SelectChip(void)
{
    WriteLines(1, 1, 0);   /* INC=HIGH, U/D=HIGH, CS=LOW */
    chipSelected = 1;
}

// ── Move wiper one tap in given direction (silencioso, sin printf por paso) ───
// direction: +1 = increment (U/D HIGH), -1 = decrement (U/D LOW)
// Step happens on the falling edge of INC (negative-edge triggered)
static void StepWiper(int direction)
{
    if (!hw_ok) return;  // no hardware, no op

    if (!chipSelected) {
        SelectChip();
    }

    uInt8 ud = (direction > 0) ? 1 : 0;

    WriteLines(1, ud, 0);   /* Step 1: set direction, INC=HIGH, CS=LOW */
    Sleep(1);               /* tCSS setup time before INC falls */

    WriteLines(0, ud, 0);   /* Step 2: INC falls → device steps wiper */
    Sleep(1);               /* tINCL: INC low pulse width */

    WriteLines(1, ud, 0);   /* Step 3: INC rises, wiper has moved */
    Sleep(1);               /* tINCH: INC high time before next op */

    potStep += direction;
    if (potStep < 0)             potStep = 0;
    if (potStep > POT_MAX_STEPS) potStep = POT_MAX_STEPS;
}

// ── Save wiper position to NVM and release CS ─────────────────────────────────
static void StoreAndDeselect(void)
{
    if (!hw_ok) return;  // no hardware, no op

    if (!chipSelected) return;

    WriteLines(1, 1, 0);   /* Ensure INC=HIGH before raising CS */
    Sleep(1);

    WriteLines(1, 1, 1);   /* Raise CS → triggers NVM store */
    Sleep(20);             /* tCP: store time ~10-20 ms */

    chipSelected = 0;
    printf("[DIST] Stored to NVM. Step=%d\n", potStep);
}

// ── Public API ────────────────────────────────────────────────────────────────

void distortion_init(void)
{
    int32 e;
    hw_ok = 0;  // asumir fallo hasta confirmar

    e = DAQmxCreateTask("distDoTask", &doTask);
    if (e < 0) {
        fprintf(stderr, "[DIST] NI no disponible — digipot deshabilitado\n");
        return;
    }

    e = DAQmxCreateDOChan(doTask,
                        "Dev2/port0/line0:2",
                        "", DAQmx_Val_ChanPerLine);
                        
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

    // ── Escritura de prueba con timeout corto (2s) ────────────────────────────
    // Si el hardware no está físicamente conectado, falla aquí y no durante
    // el uso. hw_ok permanece 0 y todo lo demás se ignora silenciosamente.
    uInt8 probe[3] = { 1, 1, 1 };  // idle state: INC=1 U/D=1 CS=1
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

    // Hardware confirmado y funcionando
    hw_ok = 1;
    printf("[DIST] Init OK — NI USB-6009 conectada\n");

    /* Select chip and move wiper to position 0 */
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
    if (t) CloseHandle(t);  // fire and forget
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