//
// Created by awalol on 2026/3/4.
//

#include <array>
#include <cstdint>

#include "hci_cmd.h"

inline const char *opcode_to_str(const uint16_t opcode) {
    switch (opcode) {
        case HCI_OPCODE_HCI_INQUIRY:
            return "HCI_INQUIRY";
        case HCI_OPCODE_HCI_INQUIRY_CANCEL:
            return "HCI_INQUIRY_CANCEL";
        case HCI_OPCODE_HCI_CREATE_CONNECTION:
            return "HCI_CREATE_CONNECTION";
        case HCI_OPCODE_HCI_ACCEPT_CONNECTION_REQUEST:
            return "HCI_ACCEPT_CONNECTION_REQUEST";
        case HCI_OPCODE_HCI_LINK_KEY_REQUEST_REPLY:
            return "HCI_LINK_KEY_REQUEST_REPLY";
        case HCI_OPCODE_HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY:
            return "HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY";
        case HCI_OPCODE_HCI_REJECT_CONNECTION_REQUEST:
            return "HCI_REJECT_CONNECTION_REQUEST";
        case HCI_OPCODE_HCI_AUTHENTICATION_REQUESTED:
            return "HCI_AUTHENTICATION_REQUESTED";
        case HCI_OPCODE_HCI_SET_CONNECTION_ENCRYPTION:
            return "HCI_SET_CONNECTION_ENCRYPTION";
        case HCI_OPCODE_HCI_READ_REMOTE_SUPPORTED_FEATURES_COMMAND:
            return "HCI_READ_REMOTE_SUPPORTED_FEATURES";
        case HCI_OPCODE_HCI_READ_REMOTE_EXTENDED_FEATURES_COMMAND:
            return "HCI_READ_REMOTE_EXTENDED_FEATURES";
        case HCI_OPCODE_HCI_SWITCH_ROLE_COMMAND:
            return "HCI_SWITCH_ROLE";
        case HCI_OPCODE_HCI_IO_CAPABILITY_REQUEST_REPLY:
            return "HCI_IO_CAPABILITY_REQUEST_REPLY";
        case HCI_OPCODE_HCI_USER_CONFIRMATION_REQUEST_REPLY:
            return "HCI_USER_CONFIRMATION_REQUEST_REPLY";
        case HCI_OPCODE_HCI_DISCONNECT:
            return "HCI_DISCONNECT";
        case HCI_OPCODE_HCI_SET_EVENT_MASK:
            return "HCI_SET_EVENT_MASK";
        case HCI_OPCODE_HCI_RESET:
            return "HCI_RESET";
        case HCI_OPCODE_HCI_WRITE_LOCAL_NAME:
            return "HCI_WRITE_LOCAL_NAME";
        case HCI_OPCODE_HCI_READ_LOCAL_NAME:
            return "HCI_READ_LOCAL_NAME";
        case HCI_OPCODE_HCI_WRITE_PAGE_TIMEOUT:
            return "HCI_WRITE_PAGE_TIMEOUT";
        case HCI_OPCODE_HCI_WRITE_SCAN_ENABLE:
            return "HCI_WRITE_SCAN_ENABLE";
        case HCI_OPCODE_HCI_WRITE_CLASS_OF_DEVICE:
            return "HCI_WRITE_CLASS_OF_DEVICE";
        case HCI_OPCODE_HCI_WRITE_INQUIRY_MODE:
            return "HCI_WRITE_INQUIRY_MODE";
        case HCI_OPCODE_HCI_WRITE_EXTENDED_INQUIRY_RESPONSE:
            return "HCI_WRITE_EXTENDED_INQUIRY_RESPONSE";
        case HCI_OPCODE_HCI_WRITE_PAGE_SCAN_TYPE:
            return "HCI_WRITE_PAGE_SCAN_TYPE";
        case HCI_OPCODE_HCI_WRITE_SIMPLE_PAIRING_MODE:
            return "HCI_WRITE_SIMPLE_PAIRING_MODE";
        case HCI_OPCODE_HCI_SET_EVENT_MASK_2:
            return "HCI_SET_EVENT_MASK_2";
        case HCI_OPCODE_HCI_WRITE_LE_HOST_SUPPORTED:
            return "HCI_WRITE_LE_HOST_SUPPORTED";
        case HCI_OPCODE_HCI_WRITE_SECURE_CONNECTIONS_HOST_SUPPORT:
            return "HCI_WRITE_SECURE_CONNECTIONS_HOST_SUPPORT";
        case HCI_OPCODE_HCI_WRITE_DEFAULT_LINK_POLICY_SETTING:
            return "HCI_WRITE_DEFAULT_LINK_POLICY_SETTING";
        case HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION:
            return "HCI_READ_LOCAL_VERSION_INFORMATION";
        case HCI_OPCODE_HCI_READ_LOCAL_SUPPORTED_COMMANDS:
            return "HCI_READ_LOCAL_SUPPORTED_COMMANDS";
        case HCI_OPCODE_HCI_READ_LOCAL_SUPPORTED_FEATURES:
            return "HCI_READ_LOCAL_SUPPORTED_FEATURES";
        case HCI_OPCODE_HCI_READ_BUFFER_SIZE:
            return "HCI_READ_BUFFER_SIZE";
        case HCI_OPCODE_HCI_READ_BD_ADDR:
            return "HCI_READ_BD_ADDR";
        case HCI_OPCODE_HCI_READ_ENCRYPTION_KEY_SIZE:
            return "HCI_READ_ENCRYPTION_KEY_SIZE";
        case HCI_OPCODE_HCI_READ_RSSI:
            return "HCI_READ_RSSI";
        case 0xFC01:
            return "HCI_VENDOR_0xFC01";
        default:
            return "UNKNOWN_OPCODE";
    }
}

inline constexpr uint32_t crc32_table_entry(uint32_t index) {
    for (unsigned bit = 0; bit < 8; ++bit) {
        index = (index >> 1) ^ (0xEDB88320 & -(index & 1));
    }
    return index;
}

inline constexpr auto make_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        table[i] = crc32_table_entry(i);
    }
    return table;
}

inline constexpr auto crc32_lookup_table = make_crc32_table();

inline uint32_t crc32_seeded(const uint8_t *data, size_t size, const uint32_t seed) {
    uint32_t crc = ~seed;

    while (size--) {
        crc = (crc >> 8) ^ crc32_lookup_table[(crc ^ *data++) & 0xff];
    }

    return ~crc;
}

inline uint32_t crc32(const uint8_t* data, size_t size) {
    return crc32_seeded(data, size, 0xEADA2D49); // 0xA2 seed
}

inline void fill_output_report_checksum(uint8_t* outputData,size_t len)
{
    uint32_t crc = crc32(outputData, len - 4);
    outputData[len - 4] = (crc >> 0) & 0xFF;
    outputData[len - 3] = (crc >> 8) & 0xFF;
    outputData[len - 2] = (crc >> 16) & 0xFF;
    outputData[len - 1] = (crc >> 24) & 0xFF;
}

inline uint32_t crc32_feature(const uint8_t *data, std::size_t size) {
    // https://github.com/rafaelvaloto/Dualsense-Multiplatform/blob/main/Source/Private/GCore/Utils/CR32.cpp
    return crc32_seeded(data, size, 0x2060efc3); // 0x53 seed
}

inline void fill_feature_report_checksum(uint8_t *data, const size_t len) {
    uint32_t crc = crc32_feature(data,len - 4);
    data[len - 4] = (crc >> 0) & 0xFF;
    data[len - 3] = (crc >> 8) & 0xFF;
    data[len - 2] = (crc >> 16) & 0xFF;
    data[len - 1] = (crc >> 24) & 0xFF;
}

/*enum PowerState : uint8_t {
    Discharging         = 0x00, // Use PowerPercent
    Charging            = 0x01, // Use PowerPercent
    Complete            = 0x02, // PowerPercent not valid? assume 100%?
    AbnormalVoltage     = 0x0A, // PowerPercent not valid?
    AbnormalTemperature = 0x0B, // PowerPercent not valid?
    ChargingError       = 0x0F  // PowerPercent not valid?
};

enum Direction : uint8_t {
    North = 0,
    NorthEast,
    East,
    SouthEast,
    South,
    SouthWest,
    West,
    NorthWest,
    None = 8
};

struct __attribute__((packed)) TouchFingerData { // 4
    /*0.0#1# uint32_t Index : 7;
    /*0.7#1# uint32_t NotTouching : 1;
    /*1.0#1# uint32_t FingerX : 12;
    /*2.4#1# uint32_t FingerY : 12;
};

struct __attribute__((packed)) TouchData { // 9
    /*0#1# TouchFingerData Finger[2];
    /*8#1# uint8_t Timestamp;
};

struct __attribute__((packed)) USBGetStateData { // 63
/* 0  #1# uint8_t LeftStickX;
/* 1  #1# uint8_t LeftStickY;
/* 2  #1# uint8_t RightStickX;
/* 3  #1# uint8_t RightStickY;
/* 4  #1# uint8_t TriggerLeft;
/* 5  #1# uint8_t TriggerRight;
/* 6  #1# uint8_t SeqNo; // always 0x01 on BT
/* 7.0#1# Direction DPad : 4;
/* 7.4#1# uint8_t ButtonSquare : 1;
/* 7.5#1# uint8_t ButtonCross : 1;
/* 7.6#1# uint8_t ButtonCircle : 1;
/* 7.7#1# uint8_t ButtonTriangle : 1;
/* 8.0#1# uint8_t ButtonL1 : 1;
/* 8.1#1# uint8_t ButtonR1 : 1;
/* 8.2#1# uint8_t ButtonL2 : 1;
/* 8.3#1# uint8_t ButtonR2 : 1;
/* 8.4#1# uint8_t ButtonCreate : 1;
/* 8.5#1# uint8_t ButtonOptions : 1;
/* 8.6#1# uint8_t ButtonL3 : 1;
/* 8.7#1# uint8_t ButtonR3 : 1;
/* 9.0#1# uint8_t ButtonHome : 1;
/* 9.1#1# uint8_t ButtonPad : 1;
/* 9.2#1# uint8_t ButtonMute : 1;
/* 9.3#1# uint8_t UNK1 : 1; // appears unused
/* 9.4#1# uint8_t ButtonLeftFunction : 1; // DualSense Edge
/* 9.5#1# uint8_t ButtonRightFunction : 1; // DualSense Edge
/* 9.6#1# uint8_t ButtonLeftPaddle : 1; // DualSense Edge
/* 9.7#1# uint8_t ButtonRightPaddle : 1; // DualSense Edge
/*10  #1# uint8_t UNK2; // appears unused
/*11  #1# uint32_t UNK_COUNTER; // Linux driver calls this reserved, tools leak calls the 2 high bytes "random"
/*15  #1# int16_t AngularVelocityX;
/*17  #1# int16_t AngularVelocityZ;
/*19  #1# int16_t AngularVelocityY;
/*21  #1# int16_t AccelerometerX;
/*23  #1# int16_t AccelerometerY;
/*25  #1# int16_t AccelerometerZ;
/*27  #1# uint32_t SensorTimestamp;
/*31  #1# int8_t Temperature; // reserved2 in Linux driver
/*32  #1# TouchData Touch;
/*41.0#1# uint8_t TriggerRightStopLocation: 4; // trigger stop can be a range from 0 to 9 (F/9.0 for Apple interface)
/*41.4#1# uint8_t TriggerRightStatus: 4;
/*42.0#1# uint8_t TriggerLeftStopLocation: 4;
/*42.4#1# uint8_t TriggerLeftStatus: 4; // 0 feedbackNoLoad
                                       // 1 feedbackLoadApplied
                                       // 0 weaponReady
                                       // 1 weaponFiring
                                       // 2 weaponFired
                                       // 0 vibrationNotVibrating
                                       // 1 vibrationIsVibrating
/*43  #1# uint32_t HostTimestamp; // mirrors data from report write
/*47.0#1# uint8_t TriggerRightEffect: 4; // Active trigger effect, previously we thought this was status max
/*47.4#1# uint8_t TriggerLeftEffect: 4;  // 0 for reset and all other effects
                                        // 1 for feedback effect
                                        // 2 for weapon effect
                                        // 3 for vibration
/*48  #1# uint32_t DeviceTimeStamp;
/*52.0#1# uint8_t PowerPercent : 4; // 0x00-0x0A
/*52.4#1# PowerState Power : 4;
/*53.0#1# uint8_t PluggedHeadphones : 1;
/*53.1#1# uint8_t PluggedMic : 1;
/*53.2#1# uint8_t MicMuted: 1; // Mic muted by powersave/mute command
/*53.3#1# uint8_t PluggedUsbData : 1;
/*53.4#1# uint8_t PluggedUsbPower : 1; // appears that this cannot be 1 if PluggedUsbData is 1
/*53.5#1# uint8_t UsbPowerOnBT : 1; // appears this is only 1 if BT connected and USB powered
/*53.5#1# uint8_t DockDetect : 1;
/*53.5#1# uint8_t PluggedUnk : 1;
/*54.0#1# uint8_t PluggedExternalMic : 1; // Is external mic active (automatic in mic auto mode)
/*54.1#1# uint8_t HapticLowPassFilter : 1; // Is the Haptic Low-Pass-Filter active?
/*54.2#1# uint8_t PluggedUnk3 : 6;
/*55  #1# uint8_t AesCmac[8];
};*/

namespace MuteLight{
    enum MuteLight : uint8_t {
        Off = 0,
        On,
        Breathing,
        DoNothing, // literally nothing, this input is ignored,
                   // though it might be a faster blink in other versions
        NoAction4,
        NoAction5,
        NoAction6,
        NoAction7= 7
    };
}

namespace LightBrightness{
    enum LightBrightness : uint8_t {
        Bright = 0,
        Mid,
        Dim,
        NoAction3,
        NoAction4,
        NoAction5,
        NoAction6,
        NoAction7= 7
    };
}

namespace LightFadeAnimation {
    enum LightFadeAnimation : uint8_t {
        Nothing = 0,
        FadeIn, // from black to blue
        FadeOut // from blue to black
    };
}

struct __attribute__((packed)) SetStateData { // 47
/*    */ // Report Set Flags
/*    */ // These flags are used to indicate what contents from this report should be processed
/* 0.0*/ uint8_t EnableRumbleEmulation: 1; // Suggest halving rumble strength
/* 0.1*/ uint8_t UseRumbleNotHaptics:   1; //
/*    */
/* 0.2*/ uint8_t AllowRightTriggerFFB: 1; // Enable setting RightTriggerFFB
/* 0.3*/ uint8_t AllowLeftTriggerFFB: 1;  // Enable setting LeftTriggerFFB
/*    */
/* 0.4*/ uint8_t AllowHeadphoneVolume: 1; // Enable setting VolumeHeadphones
/* 0.5*/ uint8_t AllowSpeakerVolume: 1;   // Enable setting VolumeSpeaker
/* 0.6*/ uint8_t AllowMicVolume: 1;       // Enable setting VolumeMic
/*    */
/* 0.7*/ uint8_t AllowAudioControl: 1; // Enable setting AudioControl section
/* 1.0*/ uint8_t AllowMuteLight: 1;    // Enable setting MuteLightMode
/* 1.1*/ uint8_t AllowAudioMute: 1;    // Enable setting MuteControl section
/*    */
/* 1.2*/ uint8_t AllowLedColor: 1; // Enable RGB LED section
/*    */
/* 1.3*/ uint8_t ResetLights: 1; // Release the LEDs from Wireless firmware control
/*    */                         // When in wireless mode this must be signaled to control LEDs
/*    */                         // This cannot be applied during the BT pair animation.
/*    */                         // SDL2 waits until the SensorTimestamp value is >= 10200000
/*    */                         // before pulsing this bit once.
/*    */
/* 1.4*/ uint8_t AllowPlayerIndicators: 1; // Enable setting PlayerIndicators section
/* 1.5*/ uint8_t AllowHapticLowPassFilter: 1; // Enable HapticLowPassFilter
/* 1.6*/ uint8_t AllowMotorPowerLevel: 1; // MotorPowerLevel reductions for trigger/haptic
/* 1.7*/ uint8_t AllowAudioControl2: 1; // Enable setting AudioControl2 section
/*    */
/* 2  */ uint8_t RumbleEmulationRight; // emulates the light weight
/* 3  */ uint8_t RumbleEmulationLeft; // emulated the heavy weight
/*    */
/* 4  */ uint8_t VolumeHeadphones; // max 0x7f
/* 5  */ uint8_t VolumeSpeaker; // PS5 appears to only use the range 0x3d-0x64
/* 6  */ uint8_t VolumeMic; // not linear, seems to max at 64, 0 is fully muted only in chat mode
/*    */
/*    */ // AudioControl
/* 7.0*/ uint8_t MicSelect: 2; // 0 Auto
/*    */                       // 1 Internal Only
/*    */                       // 2 External Only
/*    */                       // 3 Unclear, sets external mic flag but might use internal mic, do test
/* 7.2*/ uint8_t EchoCancelEnable: 1;
/* 7.3*/ uint8_t NoiseCancelEnable: 1;
/* 7.4*/ uint8_t OutputPathSelect: 2; // 0 L_R_X
/*    */                              // 1 L_L_X
/*    */                              // 2 L_L_R
/*    */                              // 3 X_X_R
/* 7.6*/ uint8_t InputPathSelect: 2;  // 0 CHAT_ASR
/*    */                              // 1 CHAT_CHAT
/*    */                              // 2 ASR_ASR
/*    */                              // 3 Does Nothing, invalid
/*    */
/* 8  */ MuteLight::MuteLight MuteLightMode;
/*    */
/*    */ // MuteControl
/* 9.0*/ uint8_t TouchPowerSave: 1;
/* 9.1*/ uint8_t MotionPowerSave: 1;
/* 9.2*/ uint8_t HapticPowerSave: 1; // AKA BulletPowerSave
/* 9.3*/ uint8_t AudioPowerSave: 1;
/* 9.4*/ uint8_t MicMute: 1;
/* 9.5*/ uint8_t SpeakerMute: 1;
/* 9.6*/ uint8_t HeadphoneMute: 1;
/* 9.7*/ uint8_t HapticMute: 1; // AKA BulletMute
/*    */
/*10  */ uint8_t RightTriggerFFB[11];
/*21  */ uint8_t LeftTriggerFFB[11];
/*32  */ uint32_t HostTimestamp; // mirrored into report read
/*    */
/*    */ // MotorPowerLevel
/*36.0*/ uint8_t RumbleMotorPowerReduction : 4; // 0x0-0x7 (no 0x8?) Applied in 12.5% reductions
/*36.4*/ uint8_t TriggerMotorPowerReduction : 4;  // 0x0-0xA
/*    */
/*    */ // AudioControl2
/*37.0*/ uint8_t SpeakerCompPreGain: 3; // additional speaker volume boost
/*37.3*/ uint8_t BeamformingEnable: 1; // Probably for MIC given there's 2, might be more bits, can't find what it does
/*37.4*/ uint8_t UnkAudioControl2: 4; // some of these bits might apply to the above
/*    */
/*38.0*/ uint8_t AllowLightBrightnessChange: 1; // LED_BRIHTNESS_CONTROL
/*38.1*/ uint8_t AllowColorLightFadeAnimation: 1; // LIGHTBAR_SETUP_CONTROL
/*38.2*/ uint8_t EnableImprovedRumbleEmulation: 1; // Use instead of EnableRumbleEmulation
                                                   // requires FW >= 0x0224
                                                   // No need to halve rumble strength
/*38.3*/ uint8_t UseRumbleNotHaptics2: 1; // 在 NinjaGaiden4 出现，与 UseRumbleNotHaptics 工作效果相同
/*38.3*/ uint8_t UNKBITC: 4; // unused
/*    */
/*39.0*/ uint8_t HapticLowPassFilter: 1;
/*39.1*/ uint8_t UNKBIT: 7;
/*    */
/*40  */ uint8_t UNKBYTE; // previous notes suggested this was HLPF, was probably off by 1
/*    */
/*41  */ LightFadeAnimation::LightFadeAnimation LightFadeAnimation;
/*42  */ LightBrightness::LightBrightness LightBrightness;
/*    */
/*    */ // PlayerIndicators
/*    */ // These bits control the white LEDs under the touch pad.
/*    */ // Note the reduction in functionality for later revisions.
/*    */ // Generation 0x03 - Full Functionality
/*    */ // Generation 0x04 - Mirrored Only
/*    */ // Suggested detection: (HardwareInfo & 0x00FFFF00) == 0X00000400
/*    */ //
/*    */ // Layout used by PS5:
/*    */ // 0x04 - -x- -  Player 1
/*    */ // 0x06 - x-x -  Player 2
/*    */ // 0x15 x -x- x  Player 3
/*    */ // 0x1B x x-x x  Player 4
/*    */ // 0x1F x xxx x  Player 5* (Unconfirmed)
/*    */ //
/*    */ //                        // HW 0x03 // HW 0x04
/*43.0*/ uint8_t PlayerLight1 : 1; // x --- - // x --- x
/*43.1*/ uint8_t PlayerLight2 : 1; // - x-- - // - x-x -
/*43.2*/ uint8_t PlayerLight3 : 1; // - -x- - // - -x- -
/*43.3*/ uint8_t PlayerLight4 : 1; // - --x - // - x-x -
/*43.4*/ uint8_t PlayerLight5 : 1; // - --- x // x --- x
/*43.5*/ uint8_t PlayerLightFade: 1; // if low player lights fade in, if high player lights instantly change
/*43.6*/ uint8_t PlayerLightUNK : 2;
/*    */
/*    */ // RGB LED
/*44  */ uint8_t LedRed;
/*45  */ uint8_t LedGreen;
/*46  */ uint8_t LedBlue;
    uint8_t reserved[16];
// Structure ends here though on BT there is padding and a CRC, see ReportOut31
};

