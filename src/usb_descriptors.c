/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 HiFiPhile
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device =
{
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // Use Interface Association Descriptor (IAD) for Audio
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    /*.bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,*/
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0x054C,
    .idProduct = 0x0CE6,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+


#define EPNUM_AUDIO       0x01

#define CONFIG_UAC1_TOTAL_LEN    	(TUD_CONFIG_DESC_LEN + TUD_AUDIO10_SPEAKER_STEREO_FB_DESC_LEN(2))

uint8_t const descriptor_configuration[] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09, // bLength
    0x02, // bDescriptorType (CONFIGURATION)
    0xE3, 0x00, // wTotalLength: 227
    0x04, // bNumInterfaces: 4
    0x01, // bConfigurationValue: 1
    0x00, // iConfiguration: 0
    0xC0, // bmAttributes: SELF-POWERED, NO REMOTE-WAKEUP
    0xFA, // bMaxPower: 500mA (250 * 2mA)

    // --- INTERFACE DESCRIPTOR (0.0): Audio Control ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x00, // bInterfaceNumber: 0
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio (0x01)
    0x01, // bInterfaceSubClass: Audio Control (0x01)
    0x00, // bInterfaceProtocol: 0x00
    0x00, // iInterface: 0

    // Class-specific AC Interface Header Descriptor
    0x0A, // bLength: 10
    0x24, // bDescriptorType: CS_INTERFACE (0x24)
    0x01, // bDescriptorSubtype: Header (0x01)
    0x00, 0x01, // bcdADC: 1.00
    0x49, 0x00, // wTotalLength: 73 (0x0049)
    0x02, // bInCollection: 2 streaming interfaces
    0x01, // baInterfaceNr(1): Interface 1
    0x02, // baInterfaceNr(2): Interface 2

    // Input Terminal Descriptor (Terminal ID 1: USB Streaming → Output to Speaker)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: Input Terminal
    0x01, // bTerminalID: 1
    0x01, 0x01, // wTerminalType: USB Streaming (0x0101)
    0x06, // bAssocTerminal: 6 (paired with USB OUT terminal)
    0x04, // bNrChannels: 4
    0x33, 0x00, // wChannelConfig: L/R Front + L/R Surround (0x0033)
    0x00, // iChannelNames: 0
    0x00, // iTerminal: 0

    // Feature Unit Descriptor (Unit ID 2 ← from Terminal 1)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x06, // bDescriptorSubtype: Feature Unit
    0x02, // bUnitID: 2
    0x01, // bSourceID: 1
    0x01, // bControlSize: 1 byte per control
    0x03, // bmaControls[0]: Master – Mute, Volume
    0x00, 0x00, 0x00, 0x00, 0x00, // bmaControls[1..4]: No per-channel controls

    // Output Terminal Descriptor (Terminal ID 3: Speaker ← from Unit 2)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x03, // bDescriptorSubtype: Output Terminal
    0x03, // bTerminalID: 3
    0x01, 0x03, // wTerminalType: Speaker (0x0301)
    0x04, // bAssocTerminal: 4 (paired with mic input)
    0x02, // bSourceID: 2 (Feature Unit)
    0x00, // iTerminal: 0

    // Input Terminal Descriptor (Terminal ID 4: Headset Mic)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: Input Terminal
    0x04, // bTerminalID: 4
    0x02, 0x04, // wTerminalType: Headset (0x0402)
    0x03, // bAssocTerminal: 3 (paired with speaker)
    0x02, // bNrChannels: 2
    0x03, 0x00, // wChannelConfig: L/R Front (0x0003)
    0x00, // iChannelNames: 0
    0x00, // iTerminal: 0

    // Feature Unit Descriptor (Unit ID 5 ← from Terminal 4)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x06, // bDescriptorSubtype: Feature Unit
    0x05, // bUnitID: 5
    0x04, // bSourceID: 4
    0x01, // bControlSize: 1
    0x03, // bmaControls[0]: Master – Mute, Volume
    0x00, // bmaControls[1]: Ch1 – no controls
    0x00, // iFeature: 0

    // Output Terminal Descriptor (Terminal ID 6: USB Streaming ← from Unit 5)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x03, // bDescriptorSubtype: Output Terminal
    0x06, // bTerminalID: 6
    0x01, 0x01, // wTerminalType: USB Streaming (0x0101)
    0x01, // bAssocTerminal: 1
    0x05, // bSourceID: 5
    0x00, // iTerminal: 0

    // --- INTERFACE DESCRIPTOR (1.0): Audio Streaming (OUT - Alternate 0) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x01, // bInterfaceNumber: 1
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // --- INTERFACE DESCRIPTOR (1.1): Audio Streaming (OUT - Alternate 1) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x01, // bInterfaceNumber: 1
    0x01, // bAlternateSetting: 1
    0x01, // bNumEndpoints: 1
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // AS General Descriptor (for Interface 1.1)
    0x07, // bLength: 7
    0x24, // bDescriptorType: CS_INTERFACE
    0x01, // bDescriptorSubtype: AS_GENERAL
    0x01, // bTerminalLink: connected to Terminal ID 1
    0x01, // bDelay: 1 frame
    0x01, 0x00, // wFormatTag: PCM (0x0001)

    // Format Type Descriptor (4-channel, 16-bit, 48kHz)
    0x0B, // bLength: 11
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: FORMAT_TYPE
    0x01, // bFormatType: TYPE_I
    0x04, // bNrChannels: 4
    0x02, // bSubframeSize: 2 bytes/sample
    0x10, // bBitResolution: 16 bits
    0x01, // bSamFreqType: 1 discrete frequency
    0x80, 0xBB, 0x00, // tSamFreq: 48000 Hz (0x00BB80)

    // Endpoint Descriptor (Audio OUT: EP1)
    0x09, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x01, // bEndpointAddress: OUT EP1
    0x09, // bmAttributes: Isochronous, Adaptive
    0x88, 0x01, // wMaxPacketSize: 392 bytes
    0x04, // bInterval: 4 (1/(2^(4-1)) ms ≈ 125 µs/frame)
    0x00, // bRefresh
    0x00, // bSynchAddress

    // Class-specific Audio Streaming Endpoint Descriptor (EP1)
    0x07, // bLength
    0x25, // bDescriptorType: CS_ENDPOINT
    0x01, // bDescriptorSubtype: GENERAL
    0x00, // Attributes: No pitch/sampling freq control
    0x00, // Lock Delay Units: Undefined
    0x00, 0x00, // Lock Delay: 0

    // --- INTERFACE DESCRIPTOR (2.0): Audio Streaming IN (Alternate 0) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x02, // bInterfaceNumber: 2
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // --- INTERFACE DESCRIPTOR (2.1): Audio Streaming IN (Alternate 1) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x02, // bInterfaceNumber: 2
    0x01, // bAlternateSetting: 1
    0x01, // bNumEndpoints: 1
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // AS General Descriptor (for Interface 2.1)
    0x07, // bLength: 7
    0x24, // bDescriptorType: CS_INTERFACE
    0x01, // bDescriptorSubtype: AS_GENERAL
    0x06, // bTerminalLink: connected to Terminal ID 6
    0x01, // bDelay: 1 frame
    0x01, 0x00, // wFormatTag: PCM (0x0001)

    // Format Type Descriptor (2-channel, 16-bit, 48kHz)
    0x0B, // bLength: 11
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: FORMAT_TYPE
    0x01, // bFormatType: TYPE_I
    0x02, // bNrChannels: 2
    0x02, // bSubframeSize: 2
    0x10, // bBitResolution: 16
    0x01, // bSamFreqType: 1
    0x80, 0xBB, 0x00, // tSamFreq: 48000 Hz

    // Endpoint Descriptor (Audio IN: EP2)
    0x09, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x82, // bEndpointAddress: IN EP2
    0x05, // bmAttributes: Isochronous, Asynchronous
    0xC4, 0x00, // wMaxPacketSize: 196 bytes
    0x04, // bInterval: 4
    0x00, // bRefresh
    0x00, // bSynchAddress

    // Class-specific Audio Streaming Endpoint Descriptor (EP2)
    0x07, // bLength
    0x25, // bDescriptorType: CS_ENDPOINT
    0x01, // bDescriptorSubtype: GENERAL
    0x00, // Attributes: No controls
    0x00, // Lock Delay Units
    0x00, 0x00, // Lock Delay

    // --- INTERFACE DESCRIPTOR (3.0): HID (DualSense 5 Gamepad + Touchpad) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x03, // bInterfaceNumber: 3
    0x00, // bAlternateSetting: 0
    0x02, // bNumEndpoints: 2 (IN + OUT)
    0x03, // bInterfaceClass: HID
    0x00, // bInterfaceSubClass: None
    0x00, // bInterfaceProtocol: None
    0x00, // iInterface

    // HID Descriptor
    0x09, // bLength: 9
    0x21, // bDescriptorType (HID)
    0x11, 0x01, // bcdHID: 1.11
    0x00, // bCountryCode: Not localized
    0x01, // bNumDescriptors: 1 report descriptor
    0x22, // bDescriptorType: Report
    0x21, 0x01, // wDescriptorLength: 289 (0x0121)

    // Endpoint Descriptor (HID IN: EP4)
    0x07, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x84, // bEndpointAddress: IN EP4
    0x03, // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize: 64
    0x06, // bInterval: 6 (polling every 8ms)

    // Endpoint Descriptor (HID OUT: EP3)
    0x07, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x03, // bEndpointAddress: OUT EP3
    0x03, // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize: 64
    0x06, // bInterval: 6
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index; // for multiple configurations
    return descriptor_configuration;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

uint8_t const desc_hid_report[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x09, 0x32, //   Usage (Z)
    0x09, 0x35, //   Usage (Rz)
    0x09, 0x33, //   Usage (Rx)
    0x09, 0x34, //   Usage (Ry)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20, //   Usage (0x20)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01, //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39, //   Usage (Hat switch)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x07, //   Logical Maximum (7)
    0x35, 0x00, //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14, //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04, //   Report Size (4)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x42, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00, //   Unit (None)
    0x05, 0x09, //   Usage Page (Button)
    0x19, 0x01, //   Usage Minimum (0x01)
    0x29, 0x0F, //   Usage Maximum (0x0F)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x0F, //   Report Count (15)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21, //   Usage (0x21)
    0x95, 0x0D, //   Report Count (13)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22, //   Usage (0x22)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x34, //   Report Count (52)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x85, 0x02, //   Report ID (2)
    0x09, 0x23, //   Usage (0x23)
    0x95, 0x2F, //   Report Count (47)
    0x91, 0x02, //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x05, //   Report ID (5)
    0x09, 0x33, //   Usage (0x33)
    0x95, 0x28, //   Report Count (40)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x08, //   Report ID (8)
    0x09, 0x34, //   Usage (0x34)
    0x95, 0x2F, //   Report Count (47)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x09, //   Report ID (9)
    0x09, 0x24, //   Usage (0x24)
    0x95, 0x13, //   Report Count (19)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x0A, //   Report ID (10)
    0x09, 0x25, //   Usage (0x25)
    0x95, 0x1A, //   Report Count (26)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x0B, 0x09, 0x41, 0x95, 0x29, 0xB1, 0x02, // ID 0B (新增项)
    0x85, 0x0C, 0x09, 0x42, 0x95, 0x29, 0xB1, 0x02, // ID 0C (新增项)
    0x85, 0x20, //   Report ID (32)
    0x09, 0x26, //   Usage (0x26)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x21, //   Report ID (33)
    0x09, 0x27, //   Usage (0x27)
    0x95, 0x04, //   Report Count (4)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x22, //   Report ID (34)
    0x09, 0x40, //   Usage (0x40)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x80, //   Report ID (128)
    0x09, 0x28, //   Usage (0x28)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x81, //   Report ID (129)
    0x09, 0x29, //   Usage (0x29)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x82, //   Report ID (130)
    0x09, 0x2A, //   Usage (0x2A)
    0x95, 0x09, //   Report Count (9)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x83, //   Report ID (131)
    0x09, 0x2B, //   Usage (0x2B)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x84, //   Report ID (132)
    0x09, 0x2C, //   Usage (0x2C)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x85, //   Report ID (133)
    0x09, 0x2D, //   Usage (0x2D)
    0x95, 0x02, //   Report Count (2)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xA0, //   Report ID (160)
    0x09, 0x2E, //   Usage (0x2E)
    0x95, 0x01, //   Report Count (1)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xE0, //   Report ID (224)
    0x09, 0x2F, //   Usage (0x2F)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF0, //   Report ID (240)
    0x09, 0x30, //   Usage (0x30)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF1, //   Report ID (241)
    0x09, 0x31, //   Usage (0x31)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF2, //   Report ID (242)
    0x09, 0x32, //   Usage (0x32)
    0x95, 0x0F, //   Report Count (15)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF4, //   Report ID (244)
    0x09, 0x35, //   Usage (0x35)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF5, //   Report ID (245)
    0x09, 0x36, //   Usage (0x36)
    0x95, 0x03, //   Report Count (3)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0, // End Collection
};

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
    (void) itf;
    return desc_hid_report;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String Descriptor Index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

// array of pointer to string descriptors
static char const *string_desc_arr[] =
{
    (const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "TinyUSB", // 1: Manufacturer
    "Wireless Controller", // 2: Product
    NULL, // 3: Serials will use unique ID if possible
    "UAC2 Speaker", // 4: Audio Interface
    "UAC1 Speaker", // 5: UAC1 Audio Interface
};

static uint16_t _desc_str[60 + 1];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL:
            chr_count = board_usb_get_serial(_desc_str + 1, 32);
            break;

        default:
            // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
            // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

            if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;

            const char *str = string_desc_arr[index];

            // Cap at max char
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
            if (chr_count > max_count) chr_count = max_count;

            // Convert ASCII string into UTF-16
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
