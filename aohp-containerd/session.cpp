/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define LOG_TAG "aohp-containerd"

#include "session.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

#include <android-base/logging.h>

namespace aohp {

void relayPtyToSocket(int ptyMasterFd, int clientFd) {
    char buf[4096];
    struct pollfd fds[2];
    fds[0].fd = ptyMasterFd;
    fds[0].events = POLLIN;
    fds[1].fd = clientFd;
    fds[1].events = POLLIN;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // PTY -> client socket
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(ptyMasterFd, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(clientFd, buf + written, n - written);
                if (w <= 0) goto done;
                written += w;
            }
        }

        // Client socket -> PTY
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(clientFd, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(ptyMasterFd, buf + written, n - written);
                if (w <= 0) goto done;
                written += w;
            }
        }

        if ((fds[0].revents | fds[1].revents) & (POLLHUP | POLLERR)) {
            break;
        }
    }

done:
    close(ptyMasterFd);
}

}  // namespace aohp
