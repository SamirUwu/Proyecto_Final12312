#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <NIDAQmx.h>


#define PACKET_SAMPLES 128
#define SAMPLE_RATE 44100
#define ADC_BITS 12
#define ADC_MAX ((1 << ADC_BITS) - 1)

#define PIPE_NAME "\\\\.\\pipe\\ni6009"

typedef struct {
    uint8_t sync[4];
    uint16_t samples[PACKET_SAMPLES];
} Packet;

uint16_t volts_to_adc(float64 volt, float64 v_min, float64 v_max) {
    float64 norm = (volt - v_min) / (v_max - v_min);
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    return (uint16_t)(norm * ADC_MAX);
}

int main(int argc, char *argv[]) {
    TaskHandle taskHandle = 0;
    HANDLE pipe_handle;
    int32 error = 0;
    char errBuff[2048] = {0};
    Packet packet;

    // Parse arguments
    const char *device = "Dev1";
    const char *channel = "ai0";
    float64 v_min = 0.0, v_max = 5.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            device = argv[++i];
        if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
            channel = argv[++i];
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "diff") == 0) {
                v_min = -10.0;
                v_max = 10.0;
            }
            i++;
        }
    }

    printf("[C feeder] NI USB-6009 | device=%s/%s | rate=%d Hz\n", device, channel, SAMPLE_RATE);
    printf("           voltage range: %.1f V -- %.1f V\n", v_min, v_max);
    printf("           packet size: %d samples\n\n", PACKET_SAMPLES);

    // Create named pipe
    pipe_handle = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, NULL);

    if (pipe_handle == INVALID_HANDLE_VALUE) {
        printf("[C feeder] ERROR: Could not create pipe\n");
        return 1;
    }

    printf("[C feeder] Waiting for reader to connect...\n");
    if (!ConnectNamedPipe(pipe_handle, NULL)) {
        printf("[C feeder] ERROR: ConnectNamedPipe failed\n");
        CloseHandle(pipe_handle);
        return 1;
    }
    printf("[C feeder] Pipe connected -- starting DAQ\n\n");

    // Create DAQmx task
    DAQmxCreateTask("", &taskHandle);

    char channel_str[256];
    snprintf(channel_str, sizeof(channel_str), "%s/%s", device, channel);

    DAQmxCreateAIVoltageChan(taskHandle, channel_str, "", DAQmx_Val_RSE,
                              v_min, v_max, DAQmx_Val_Volts, NULL);

    // Large hardware buffer to absorb any processing jitter
    DAQmxCfgSampClkTiming(taskHandle, "", SAMPLE_RATE, DAQmx_Val_Rising,
                          DAQmx_Val_ContSamps, PACKET_SAMPLES * 16);

    // Sequential reading from current position — no jumps, no repeats
DAQmxSetReadRelativeTo(taskHandle, DAQmx_Val_CurrReadPos);  // was DAQmx_Val_CurrentReadPosition
    DAQmxSetReadOffset(taskHandle, 0);

    DAQmxStartTask(taskHandle);
    printf("[C feeder] DAQ task running\n");

    // Set sync word
    packet.sync[0] = 0xAA;
    packet.sync[1] = 0x55;
    packet.sync[2] = 0xFF;
    packet.sync[3] = 0x00;

    float64 *sample_buffer = (float64*)malloc(PACKET_SAMPLES * sizeof(float64));
    if (!sample_buffer) {
        printf("[C feeder] ERROR: Could not allocate buffer\n");
        DAQmxStopTask(taskHandle);
        DAQmxClearTask(taskHandle);
        CloseHandle(pipe_handle);
        return 1;
    }

    int total_packets = 0;
    int32 samples_read = 0;

    while (1) {
        // Block until exactly PACKET_SAMPLES are available sequentially
        error = DAQmxReadAnalogF64(taskHandle, PACKET_SAMPLES, 0.5,
                                   DAQmx_Val_GroupByChannel, sample_buffer,
                                   PACKET_SAMPLES, &samples_read, NULL);

        if (error != 0) {
            DAQmxGetExtendedErrorInfo(errBuff, 2048);
            printf("[C feeder] DAQmx error: %s\n", errBuff);
            break;
        }

        for (int i = 0; i < PACKET_SAMPLES; i++)
            packet.samples[i] = volts_to_adc(sample_buffer[i], v_min, v_max);

        DWORD bytes_written;
        if (!WriteFile(pipe_handle, &packet, sizeof(packet), &bytes_written, NULL)) {
            printf("[C feeder] Pipe write error -- reader closed\n");
            break;
        }

        total_packets++;
        if (total_packets % 344 == 0)
            printf("[C feeder] %d packets sent\n", total_packets);
    }

    // Cleanup
    free(sample_buffer);
    DAQmxStopTask(taskHandle);
    DAQmxClearTask(taskHandle);
    CloseHandle(pipe_handle);
    printf("[C feeder] done\n");

    return 0;
}