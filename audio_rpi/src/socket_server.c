#include "socket_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 54321

static SOCKET server_fd = INVALID_SOCKET;
static SOCKET client_fd = INVALID_SOCKET;

int socket_init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(TCP_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[socket] ERROR: bind failed\n");
        return -1;
    }
    if (listen(server_fd, 1) != 0) {
        printf("[socket] ERROR: listen failed\n");
        return -1;
    }

    printf("Waiting for Python client on TCP port %d...\n", TCP_PORT);
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == INVALID_SOCKET) {
        printf("[socket] ERROR: accept failed\n");
        return -1;
    }
    printf("Client connected\n");

    // Non-blocking so send() never stalls the audio thread
    u_long mode = 1;
    ioctlsocket(client_fd, FIONBIO, &mode);

    return 0;
}

int socket_receive(char *buffer, int max_len) {
    int n = recv(client_fd, buffer, max_len - 1, 0);
    if (n > 0) buffer[n] = '\0';
    return n;
}

/**
 * socket_receive_timeout - Non-blocking receive with timeout
 * @buffer: Output buffer for received data
 * @max_len: Maximum buffer size
 * @timeout_ms: Timeout in milliseconds (0 = non-blocking, -1 = blocking)
 * 
 * Returns:
 *   > 0 : bytes received
 *   = 0 : timeout (no data available)
 *   < 0 : error or connection closed
 */
int socket_receive_timeout(char *buffer, int max_len, int timeout_ms) {
    if (client_fd == INVALID_SOCKET) return -1;

    fd_set readfds;
    struct timeval tv;
    
    // Configure timeout
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    FD_ZERO(&readfds);
    FD_SET(client_fd, &readfds);
    
    // Wait for socket to be readable with timeout
    int activity = select(0, &readfds, NULL, NULL, &tv);
    
    if (activity < 0) {
        // Error in select
        return -1;
    } else if (activity == 0) {
        // Timeout - no data available
        return 0;
    }
    
    // Data is available, receive it
    int n = recv(client_fd, buffer, max_len - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
    }
    return n;
}

int socket_send_two_floats(float pre, float post) {
    float buf[2] = { pre, post };
    return send(client_fd, (char*)buf, sizeof(buf), 0);
}

int socket_send_batch(const float *pre, const float *post, int n) {
    float *interleaved = (float*)malloc(n * 2 * sizeof(float));
    if (!interleaved) return -1;

    for (int i = 0; i < n; i++) {
        interleaved[i*2]   = pre[i];
        interleaved[i*2+1] = post[i];
    }

    int total = n * 2 * sizeof(float);
    int sent  = send(client_fd, (char*)interleaved, total, 0);
    free(interleaved);

    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAENOTCONN)
            client_fd = INVALID_SOCKET;
    }
    return sent;
}

void socket_close() {
    if (client_fd != INVALID_SOCKET) closesocket(client_fd);
    if (server_fd != INVALID_SOCKET) closesocket(server_fd);
    WSACleanup();
}