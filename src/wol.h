//
// Wake-on-LAN over WiFi (ENABLE_WOL).
//
// When the controller connects (the L2CAP HID Interrupt channel opens), WiFi is
// brought up in station mode, a "magic packet" is sent to the PC's MAC defined
// in secrets.h and WiFi is brought back down. Everything is done in a NON-blocking
// way (state machine serviced from the main loop) so as not to trip the watchdog
// and not to interfere with the controller's Bluetooth.
//

#ifndef DS5_BRIDGE_WOL_H
#define DS5_BRIDGE_WOL_H

// Initializes the internal state and validates the secrets.h credentials.
// With template placeholders (or an unparsable MAC) WoL disables itself for
// the whole session. Call once, after cyw43_arch_init().
void wol_init();

// Requests a WoL sequence. Safe to call from the Bluetooth callback context:
// it only flags a request; the actual work (WiFi/UDP) is done in
// wol_tick(). It has internal debouncing so it does not repeat on fast reconnects.
void wol_request();

// Advances the state machine. Call on every iteration of the main loop.
void wol_tick();

#endif // DS5_BRIDGE_WOL_H
