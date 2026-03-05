//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#include "tusb.h"

// typedef void (*usb_hid_get_report_callback_t)(uint16_t channel, uint8_t *data, uint16_t len);
// typedef void (*usb_hid_set_report_callback_t)(uint16_t channel, uint8_t *data, uint16_t len);

/*uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen);
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize);*/
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request);
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf);
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len);

#endif //DS5_BRIDGE_USB_H