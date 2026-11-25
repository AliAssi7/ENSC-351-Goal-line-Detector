#ifndef GPIO_H
#define GPIO_H

namespace gpio {

// Lines are given by their BeagleY-AI header name (e.g. "GPIO17"),
// or directly as "gpiochipN:line". Returns a line fd, or -1 on error.
int openInput(const char* name);
int openOutput(const char* name, int value);

// 1 = high, 0 = low, -1 = error
int get(int fd);
bool set(int fd, int value);

// Waits for an edge on any of the lines. Returns false on timeout.
bool wait(const int* fds, int count, int timeoutMs);

void close(int fd);

}

#endif
