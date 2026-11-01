//
// Elastic buffer between the controller's mic clock and the USB side.
//

#ifndef DS5_BRIDGE_ELASTIC_H
#define DS5_BRIDGE_ELASTIC_H

#include <cstdint>

// Decoded controller mic samples go in as they arrive.
void mic_elastic_write(const int16_t *samples, int count);
// Reads count mono samples; false while the buffer refills (send silence then).
bool mic_elastic_read(int16_t *out, int count);

#endif //DS5_BRIDGE_ELASTIC_H
