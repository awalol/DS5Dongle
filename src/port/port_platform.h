#pragma once

// Platform selection. Kept free of C++ so C translation units (ram_mem.c) can
// pull in port_mem.h for the placement macros without dragging in the rest.
//
// Exactly one PORT_PLATFORM_* macro is defined, driven by what the build system
// already tells us.

#if defined(PICO_ON_DEVICE) || defined(PICO_SDK_VERSION_MAJOR)
#define PORT_PLATFORM_PICO 1
#elif defined(ESP_PLATFORM)
#define PORT_PLATFORM_ESP32S31 1
#else
#error "port: unknown platform; expected pico-sdk or ESP-IDF"
#endif
