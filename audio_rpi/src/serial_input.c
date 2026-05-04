#include "serial_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// ─── Protocolo (debe coincidir con el sketch Arduino y el monitor Python) ────
static const uint8_t SYNC_WORD[4] = {0xAA, 0x55, 0xFF, 0x00};

// ─── Tabla interna de handles (mapea fd entero → HANDLE de Windows) ──────────
#define MAX_SERIAL_PORTS 8
static HANDLE serial_handles[MAX_SERIAL_PORTS];
static int    serial_fd_count = 0;

// ─── Autodetectar puerto COM ──────────────────────────────────────────────────
// Prueba COM1-COM32 y devuelve el primero que abre correctamente.
// Devuelve un puntero estático válido hasta la próxima llamada, o NULL si no
// encuentra ninguno.
// ─── Autodetectar puerto COM de la NI USB-6009 ───────────────────────────────
// Busca en el registro de Windows el puerto COM asignado al driver NI.
// Devuelve puntero estático válido hasta la próxima llamada, o NULL si falla.
const char *serial_autodetect(void)
{
    static char port_name[16];

    // Subclave donde Windows registra los puertos COM por nombre de dispositivo
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        fprintf(stderr, "[serial] no se pudo abrir el registro SERIALCOMM\n");
        return NULL;
    }

    DWORD index = 0;
    char  value_name[256];
    char  value_data[64];
    DWORD name_len, data_len, type;

    while (1) {
        name_len = sizeof(value_name);
        data_len = sizeof(value_data);

        LONG ret = RegEnumValueA(hKey, index++,
                                 value_name, &name_len,
                                 NULL, &type,
                                 (LPBYTE)value_data, &data_len);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS)       continue;

        // El nombre del valor contiene el nombre del driver.
        // El driver NI-DAQmx registra entradas que contienen "usbser" o "NI"
        // Ejemplo de value_name real: \Device\USBSER000  o  \Device\NIDAQmx...
        int is_ni = 0;

        // Coincidencias conocidas del driver NI USB-6009
        const char *ni_patterns[] = {
            "NI",       // NIDAQmx virtualiza el puerto así
            "usbser",   // clase genérica USB serial que NI también usa
            "USB6009",
            "usb-6009",
            NULL
        };
        for (int p = 0; ni_patterns[p] != NULL; p++) {
            // Comparación case-insensitive manual (no hay strcasestr en MSVC)
            char lower_name[256];
            strncpy(lower_name, value_name, sizeof(lower_name) - 1);
            lower_name[sizeof(lower_name) - 1] = '\0';
            for (int c = 0; lower_name[c]; c++)
                if (lower_name[c] >= 'A' && lower_name[c] <= 'Z')
                    lower_name[c] += 32;

            char lower_pat[64];
            strncpy(lower_pat, ni_patterns[p], sizeof(lower_pat) - 1);
            lower_pat[sizeof(lower_pat) - 1] = '\0';
            for (int c = 0; lower_pat[c]; c++)
                if (lower_pat[c] >= 'A' && lower_pat[c] <= 'Z')
                    lower_pat[c] += 32;

            if (strstr(lower_name, lower_pat)) { is_ni = 1; break; }
        }

        if (!is_ni) continue;

        // value_data contiene "COMx"
        strncpy(port_name, value_data, sizeof(port_name) - 1);
        port_name[sizeof(port_name) - 1] = '\0';

        RegCloseKey(hKey);
        printf("[serial] NI USB-6009 detectada en: %s  (driver: %s)\n",
               port_name, value_name);
        return port_name;
    }

    RegCloseKey(hKey);

    // ── Fallback: si el registro no tiene el patrón, intentar por descripción
    //    en la rama Enum\USB buscando VID/PID de la NI USB-6009
    //    VID: 0x3923  PID: 0x7272  (valores oficiales NI)
    fprintf(stderr, "[serial] NI USB-6009 no encontrada en SERIALCOMM, "
                    "probando por VID/PID...\n");

    HKEY hEnum;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB",
                      0, KEY_READ, &hEnum) == ERROR_SUCCESS) {

        // Buscar VID_3923&PID_7272
        DWORD vi = 0;
        char  vid_key[64];
        DWORD vid_len = sizeof(vid_key);

        while (RegEnumKeyExA(hEnum, vi++, vid_key, &vid_len,
                             NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            vid_len = sizeof(vid_key);

            // ¿Es el VID/PID de NI USB-6009?
            char low[64];
            strncpy(low, vid_key, sizeof(low)-1);
            for (int c = 0; low[c]; c++)
                if (low[c] >= 'A' && low[c] <= 'Z') low[c] += 32;

            if (!strstr(low, "vid_3923")) continue; // VID de National Instruments

            // Abrir subclaves (instancias)
            char full_path[128];
            snprintf(full_path, sizeof(full_path),
                     "SYSTEM\\CurrentControlSet\\Enum\\USB\\%s", vid_key);

            HKEY hVid;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full_path,
                              0, KEY_READ, &hVid) != ERROR_SUCCESS) continue;

            DWORD ii = 0;
            char  inst[128];
            DWORD inst_len = sizeof(inst);
            while (RegEnumKeyExA(hVid, ii++, inst, &inst_len,
                                 NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                inst_len = sizeof(inst);

                char param_path[256];
                snprintf(param_path, sizeof(param_path),
                         "SYSTEM\\CurrentControlSet\\Enum\\USB\\%s\\%s"
                         "\\Device Parameters",
                         vid_key, inst);

                HKEY hParam;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, param_path,
                                  0, KEY_READ, &hParam) != ERROR_SUCCESS) continue;

                char port_val[16];
                DWORD pv_len = sizeof(port_val);
                if (RegQueryValueExA(hParam, "PortName", NULL, NULL,
                                     (LPBYTE)port_val, &pv_len) == ERROR_SUCCESS) {
                    strncpy(port_name, port_val, sizeof(port_name)-1);
                    port_name[sizeof(port_name)-1] = '\0';
                    RegCloseKey(hParam);
                    RegCloseKey(hVid);
                    RegCloseKey(hEnum);
                    printf("[serial] NI USB-6009 (VID/PID) detectada en: %s\n", port_name);
                    return port_name;
                }
                RegCloseKey(hParam);
            }
            RegCloseKey(hVid);
        }
        RegCloseKey(hEnum);
    }

    fprintf(stderr, "[serial] NI USB-6009 no encontrada por ningun metodo\n");
    return NULL;
}

// ─── Abrir y configurar el puerto COM ────────────────────────────────────────
// Si port == NULL, autodetecta.
// Devuelve un fd (índice en tabla interna) o -1 en error.
int serial_open(const char *port, int baud)
{
    if (port == NULL) {
        port = serial_autodetect();
        if (port == NULL) return -1;
    }

    if (serial_fd_count >= MAX_SERIAL_PORTS) {
        fprintf(stderr, "[serial] demasiados puertos abiertos\n");
        return -1;
    }

    // Usar prefijo \\.\COMx para compatibilidad con puertos > COM9
    char dev_name[32];
    snprintf(dev_name, sizeof(dev_name), "\\\\.\\%s", port);

    HANDLE h = CreateFileA(dev_name, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[serial] no se pudo abrir %s: error %lu\n",
                port, GetLastError());
        return -1;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "[serial] GetCommState: error %lu\n", GetLastError());
        CloseHandle(h);
        return -1;
    }

    // 8N1, sin control de flujo, baud rate configurable
    dcb.BaudRate        = (DWORD)baud;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = ONESTOPBIT;
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_DISABLE;
    dcb.fRtsControl     = RTS_CONTROL_DISABLE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;

    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "[serial] SetCommState: error %lu\n", GetLastError());
        CloseHandle(h);
        return -1;
    }

    // Timeout de lectura: 2 segundos total
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadTotalTimeoutConstant    = 2000; // ms
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadIntervalTimeout         = 0;
    timeouts.WriteTotalTimeoutConstant   = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    if (!SetCommTimeouts(h, &timeouts)) {
        fprintf(stderr, "[serial] SetCommTimeouts: error %lu\n", GetLastError());
        CloseHandle(h);
        return -1;
    }

    // Vaciar buffer de entrada antes de empezar
    Sleep(100);  // 100 ms: el ESP32 puede estar enviando basura al conectar
    PurgeComm(h, PURGE_RXCLEAR);

    printf("[serial] abierto %s @ %d baud\n", port, baud);
    int fd = serial_fd_count++;
    serial_handles[fd] = h;
    return fd;
}

void serial_close(int fd)
{
    if (fd >= 0 && fd < MAX_SERIAL_PORTS && serial_handles[fd] != INVALID_HANDLE_VALUE) {
        CloseHandle(serial_handles[fd]);
        serial_handles[fd] = INVALID_HANDLE_VALUE;
    }
}

// ─── Leer exactamente n bytes (bloqueante con timeout) ───────────────────────
static int read_exact(HANDLE h, uint8_t *buf, DWORD n)
{
    DWORD total = 0;
    while (total < n) {
        DWORD r = 0;
        if (!ReadFile(h, buf + total, n - total, &r, NULL) || r == 0)
            return -1;  // timeout o error
        total += r;
    }
    return 0;
}

// ─── Buscar sync word con ventana deslizante ─────────────────────────────────
static int find_sync(HANDLE h)
{
    uint8_t window[4] = {0};
    // Intentamos hasta 8 * PACKET_PAYLOAD bytes antes de rendirse
    int max_tries = SERIAL_PACKET_SAMPLES * 8 * 2;
    for (int t = 0; t < max_tries; t++) {
        uint8_t b;
        DWORD r = 0;
        if (!ReadFile(h, &b, 1, &r, NULL) || r == 0) return -1;
        // Desplazar ventana
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = b;
        if (memcmp(window, SYNC_WORD, 4) == 0) return 0;
    }
    fprintf(stderr, "[serial] sync no encontrado tras %d bytes\n", max_tries);
    return -1;
}

// ─── Leer un paquete completo ─────────────────────────────────────────────────
int serial_read_packet(int fd, uint16_t *out_samples)
{
    if (fd < 0 || fd >= MAX_SERIAL_PORTS || serial_handles[fd] == INVALID_HANDLE_VALUE) return -1;
    HANDLE h = serial_handles[fd];

    if (find_sync(h) < 0) return -1;

    uint8_t raw[SERIAL_PACKET_SAMPLES * 2];
    if (read_exact(h, raw, (DWORD)sizeof(raw)) < 0) {
        fprintf(stderr, "[serial] timeout leyendo payload\n");
        return -1;
    }

    // Decodificar little-endian → uint16
    for (int i = 0; i < SERIAL_PACKET_SAMPLES; i++) {
        out_samples[i] = (uint16_t)(raw[i * 2]) | ((uint16_t)(raw[i * 2 + 1]) << 8);
    }
    return 0;
}
