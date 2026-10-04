/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <string>

namespace aohp {

// Relay data between a client socket fd and a PTY master fd until one side
// closes.  Runs in calling thread (blocking).
void relayPtyToSocket(int ptyMasterFd, int clientFd);

}  // namespace aohp
