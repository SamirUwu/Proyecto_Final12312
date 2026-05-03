#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <NIDAQmx.h>        
#include "../include/distortion.h"

#define POT_MAX_STEPS 99

static int        potStep      = 0;
static int        chipSelected = 0;
static TaskHandle doTask       = 0;

// ── Raw line writer ───────────────────────────────────────────────────────────
// line0 = INC, line1 = U/D, line2 = CS
static int WriteLines(uInt8 inc, uInt8 ud, uInt8 cs)
{
    uInt8 data[3] = { inc, ud, cs };
    int32 e = DAQmxWriteDigitalLines(doTask, 1, 1, 10.0,
                                     DAQmx_Val_GroupByChannel,
                                     data, NULL, NULL);
    if (e < 0) {
        char eb[256];
        DAQmxGetExtendedErrorInfo(eb, sizeof(eb));
        fprintf(stderr, "[DIST ERR] WriteDigital: %d  %s\n", (int)e, eb);
        return 0;
    }
    return 1;
}

// ── Assert CS LOW so device starts listening ──────────────────────────────────
static void SelectChip(void)
{
    WriteLines(1, 1, 0);   /* INC=HIGH, U/D=HIGH, CS=LOW */
    chipSelected = 1;
    printf("[DIST] Chip selected (CS=LOW)\n");
}

// ── Move wiper one tap in given direction ─────────────────────────────────────
// direction: +1 = increment (U/D HIGH), -1 = decrement (U/D LOW)
// Step happens on the falling edge of INC (negative-edge triggered)
static void StepWiper(int direction)
{
    if (!chipSelected) {
        printf("[DIST WARN] StepWiper called while chip not selected — selecting first\n");
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

    printf("[DIST] Wiper step %+d -> position %d\n", direction, potStep);
}

// ── Save wiper position to NVM and release CS ─────────────────────────────────
static void StoreAndDeselect(void)
{
    if (!chipSelected) {
        printf("[DIST WARN] StoreAndDeselect: chip not selected, nothing to store\n");
        return;
    }

    WriteLines(1, 1, 0);   /* Ensure INC=HIGH before raising CS */
    Sleep(1);

    WriteLines(1, 1, 1);   /* Raise CS → triggers NVM store */
    Sleep(20);             /* tCP: store time ~10-20 ms */

    chipSelected = 0;
    printf("[DIST] Stored to NVM and deselected. Step=%d\n", potStep);
}

// ── Public API ────────────────────────────────────────────────────────────────

void distortion_init(void)
{
    int32 e;

    e = DAQmxCreateTask("distDoTask", &doTask);
    if (e < 0) { fprintf(stderr, "[DIST ERR] CreateTask failed: %d\n", (int)e); return; }

    e = DAQmxCreateDOChan(doTask,
                          "Dev1/port0/line0,"   /* INC */
                          "Dev1/port0/line1,"   /* U/D */
                          "Dev1/port0/line2",   /* CS  */
                          "", DAQmx_Val_ChanPerLine);
    if (e < 0) { fprintf(stderr, "[DIST ERR] CreateDOChan failed: %d\n", (int)e); return; }

    e = DAQmxStartTask(doTask);
    if (e < 0) { fprintf(stderr, "[DIST ERR] StartTask failed: %d\n", (int)e); return; }

    /* Idle state: all lines HIGH (chip deselected) */
    WriteLines(1, 1, 1);
    printf("[DIST] Init OK — idle (INC=1 U/D=1 CS=1)\n");

    /* Select chip and move wiper to position 0 */
    SelectChip();
    while (potStep > 0)
        StepWiper(-1);

    printf("[DIST] Wiper reset to position 0\n");
}

void distortion_set_volume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    int target = (int)(volume * POT_MAX_STEPS);
    int delta  = target - potStep;

    if (delta == 0) return; /* already there, no steps needed */

    int dir = (delta > 0) ? +1 : -1;

    SelectChip();
    int steps = abs(delta);
    for (int i = 0; i < steps; i++)
        StepWiper(dir);

    printf("[DIST] Volume set to %.2f -> wiper position %d\n", volume, potStep);
}

void distortion_store(void)
{
    StoreAndDeselect();

    if (doTask) {
        DAQmxStopTask(doTask);
        DAQmxClearTask(doTask);
        doTask = 0;
    }
    printf("[DIST] Shutdown complete\n");
}