#pragma once
// Host-test stub: no init-provided socket; the daemon falls back to --bind PATH.
inline int android_get_control_socket(const char*) { return -1; }
