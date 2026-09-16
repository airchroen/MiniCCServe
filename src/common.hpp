#pragma once

// Shared platform includes for MiniCCServe.
// winsock2.h must be included before windows.h (winsock version conflict).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
