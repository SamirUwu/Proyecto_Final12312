#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#include <stdint.h>

int  socket_init(void);
int  socket_receive(char *buffer, int max_len);
int  socket_receive_timeout(char *buffer, int max_len, int timeout_ms);
int  socket_send_two_floats(float pre, float post);
int  socket_send_batch(const float *pre, const float *post, int n);
void socket_close(void);

#endif