#pragma once

// Platform abstraction layer.
//
// The Pico backends are inline wrappers over the pico-sdk, so migrating a call
// site to the port API must not change codegen — that matters because
// cmake/relocate_to_ram.cmake tunes this firmware by pulling specific hot
// functions into RAM.

#include "port_platform.h"
#include "port_mem.h"
#include "port_time.h"
#include "port_timer.h"
#include "port_sync.h"
#include "port_queue.h"
#include "port_sys.h"
#include "port_gpio.h"
#include "port_led.h"
#include "port_thread.h"
#include "port_usb.h"
#include "port_flash.h"
