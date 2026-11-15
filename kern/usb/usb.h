#ifndef PRONINX_USB_USB_H
#define PRONINX_USB_USB_H

// USB 2

#include "inc/types.h"

#define USB_SPEED_LOW 1
#define USB_SPEED_FULL 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4

#define USB_MAX_DEPTH 6
#define USB_MAX_PORTS 255
#define USB_PORT_POWERUP_DELAY 100 // ms, 7.1.7.3
#define USB_MAX_ENDPOINTS 8
#define USB_MAX_INTERFACES 8

struct usb_device_request {
  uint8_t bmRequestType;
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
} __packed;

// bmRequestType
#define UT_WRITE 0x00
#define UT_READ 0x80
#define UT_STANDARD 0x00
#define UT_CLASS 0x20
#define UT_VENDOR 0x40
#define UT_DEVICE 0x00
#define UT_INTERFACE 0x01
#define UT_ENDPOINT 0x02
#define UT_OTHER 0x03

#define UT_READ_DEVICE (UT_READ | UT_STANDARD | UT_DEVICE)
#define UT_WRITE_DEVICE (UT_WRITE | UT_STANDARD | UT_DEVICE)
#define UT_READ_INTERFACE (UT_READ | UT_STANDARD | UT_INTERFACE)
#define UT_WRITE_INTERFACE (UT_WRITE | UT_STANDARD | UT_INTERFACE)
#define UT_WRITE_ENDPOINT (UT_WRITE | UT_STANDARD | UT_ENDPOINT)
#define UT_READ_CLASS_DEVICE (UT_READ | UT_CLASS | UT_DEVICE)
#define UT_WRITE_CLASS_DEVICE (UT_WRITE | UT_CLASS | UT_DEVICE)
#define UT_READ_CLASS_OTHER (UT_READ | UT_CLASS | UT_OTHER)
#define UT_WRITE_CLASS_OTHER (UT_WRITE | UT_CLASS | UT_OTHER)
#define UT_READ_CLASS_INTERFACE (UT_READ | UT_CLASS | UT_INTERFACE)
#define UT_WRITE_CLASS_INTERFACE (UT_WRITE | UT_CLASS | UT_INTERFACE)
#define UT_WRITE_CLASS_ENDPOINT (UT_WRITE | UT_CLASS | UT_ENDPOINT)

// bRequest
#define UR_GET_STATUS 0x00
#define UR_CLEAR_FEATURE 0x01
#define UR_SET_FEATURE 0x03
#define UR_SET_ADDRESS 0x05
#define UR_GET_DESCRIPTOR 0x06
#define UR_SET_DESCRIPTOR 0x07
#define UR_GET_CONFIG 0x08
#define UR_SET_CONFIG 0x09
#define UR_GET_INTERFACE 0x0a
#define UR_SET_INTERFACE 0x0b
#define UR_SET_SEL 0x30
#define UR_SET_CUR 0x01

// descriptor types
#define UDESC_DEVICE 0x01
#define UDESC_CONFIG 0x02
#define UDESC_STRING 0x03
#define UDESC_INTERFACE 0x04
#define UDESC_ENDPOINT 0x05
#define UDESC_DEVICE_QUALIFIER 0x06
#define UDESC_INTERFACE_ASSOC 0x0b
#define UDESC_HID 0x21
#define UDESC_REPORT 0x22
#define UDESC_HUB 0x29
#define UDESC_SS_HUB 0x2a
#define UDESC_ENDPOINT_SS_COMP 0x30

struct usb_device_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bcdUSB;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bMaxPacketSize0;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t iManufacturer;
  uint8_t iProduct;
  uint8_t iSerialNumber;
  uint8_t bNumConfigurations;
} __packed;

struct usb_config_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t wTotalLength;
  uint8_t bNumInterface;
  uint8_t bConfigurationValue;
  uint8_t iConfiguration;
  uint8_t bmAttributes;
  uint8_t bMaxPower;
} __packed;

struct usb_interface_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t iInterface;
} __packed;

struct usb_endpoint_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bEndpointAddress;
  uint8_t bmAttributes;
  uint16_t wMaxPacketSize;
  uint8_t bInterval;
} __packed;

#define UE_DIR_IN 0x80
#define UE_ADDR 0x0f
#define UE_XFERTYPE 0x03
#define UE_CONTROL 0x00
#define UE_ISOCHRONOUS 0x01
#define UE_BULK 0x02
#define UE_INTERRUPT 0x03

#define UE_GET_DIR(a) ((a) & UE_DIR_IN)
#define UE_GET_ADDR(a) ((a) & UE_ADDR)
#define UE_GET_XFERTYPE(a) ((a) & UE_XFERTYPE)

// superspeed endpoint companion
struct usb_ss_ep_comp_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bMaxBurst;
  uint8_t bmAttributes;
  uint16_t wBytesPerInterval;
} __packed;

// hub class
#define UCLASS_AUDIO 0x01
#define UCLASS_HUB 0x09
#define UCLASS_HID 0x03
#define UCLASS_MASS 0x08
#define UCLASS_VENDOR 0xff

#define USUBCLASS_BOOT 0x01
#define UPROTO_BOOT_KEYBOARD 0x01
#define UPROTO_BOOT_MOUSE 0x02

#define USUBCLASS_SCSI 0x06
#define UPROTO_MASS_BBB 0x50

// AUDIO CLASS 1
#define USUBCLASS_AUDIOCONTROL 0x01
#define USUBCLASS_AUDIOSTREAM 0x02
#define UDESC_CS_INTERFACE 0x24
#define UDESC_CS_ENDPOINT 0x25
#define UAUDIO_AS_GENERAL 0x01
#define UAUDIO_FORMAT_TYPE 0x02
#define UAUDIO_EP_GENERAL 0x01
#define UAUDIO_FORMAT_TYPE_I 0x01
#define UAUDIO_FORMAT_PCM 0x0001
#define UAUDIO_SAMPLING_FREQ_CONTROL 0x01
#define UAUDIO_FREQ_CONTROL_BIT 0x01

struct uaudio_as_general {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubtype;
  uint8_t bTerminalLink;
  uint8_t bDelay;
  uint16_t wFormatTag;
} __packed;

struct uaudio_format_type_i {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubtype;
  uint8_t bFormatType;
  uint8_t bNrChannels;
  uint8_t bSubframeSize;
  uint8_t bBitResolution;
  uint8_t bSamFreqType;
  uint8_t tSamFreq[3];
} __packed;

struct uaudio_ep_general {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubtype;
  uint8_t bmAttributes;
  uint8_t bLockDelayUnits;
  uint16_t wLockDelay;
} __packed;

struct usb_hub_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bNbrPorts;
  uint16_t wHubCharacteristics;
  uint8_t bPwrOn2PwrGood;
  uint8_t bHubContrCurrent;
  uint8_t DeviceRemovable[32];
} __packed;

#define UHD_PWR 0x0003
#define UHD_PWR_INDIVIDUAL 0x0001
#define UHD_TT_THINK 0x0060

// hub class feature selectors
#define UHF_C_HUB_LOCAL_POWER 0
#define UHF_C_HUB_OVER_CURRENT 1
#define UHF_PORT_CONNECTION 0
#define UHF_PORT_ENABLE 1
#define UHF_PORT_SUSPEND 2
#define UHF_PORT_OVER_CURRENT 3
#define UHF_PORT_RESET 4
#define UHF_PORT_LINK_STATE 5
#define UHF_PORT_POWER 8
#define UHF_PORT_LOW_SPEED 9
#define UHF_C_PORT_CONNECTION 16
#define UHF_C_PORT_ENABLE 17
#define UHF_C_PORT_SUSPEND 18
#define UHF_C_PORT_OVER_CURRENT 19
#define UHF_C_PORT_RESET 20
#define UHF_PORT_TEST 21
#define UHF_PORT_INDICATOR 22
#define UHF_C_PORT_LINK_STATE 25
#define UHF_BH_PORT_RESET 28
#define UHF_C_BH_PORT_RESET 29

struct usb_port_status {
  uint16_t wPortStatus;
  uint16_t wPortChange;
} __packed;

#define UPS_CURRENT_CONNECT_STATUS 0x0001
#define UPS_PORT_ENABLED 0x0002
#define UPS_SUSPEND 0x0004
#define UPS_OVERCURRENT_INDICATOR 0x0008
#define UPS_RESET 0x0010
#define UPS_PORT_L1 0x0020
#define UPS_PORT_POWER 0x0100
#define UPS_PORT_POWER_SS 0x0200
#define UPS_LOW_SPEED 0x0200
#define UPS_HIGH_SPEED 0x0400
#define UPS_PORT_MODE_DEVICE 0x8000
#define UPS_OTHER_SPEED 0x0600

#define UPS_C_CONNECT_STATUS 0x0001
#define UPS_C_PORT_ENABLED 0x0002
#define UPS_C_SUSPEND 0x0004
#define UPS_C_OVERCURRENT_INDICATOR 0x0008
#define UPS_C_PORT_RESET 0x0010
#define UPS_C_BH_PORT_RESET 0x0020
#define UPS_C_PORT_LINK_STATE 0x0040

// hid class requests
#define UR_GET_REPORT 0x01
#define UR_GET_IDLE 0x02
#define UR_GET_PROTOCOL 0x03
#define UR_SET_REPORT 0x09
#define UR_SET_IDLE 0x0a
#define UR_SET_PROTOCOL 0x0b

#define UHID_BOOT_PROTOCOL 0
#define UHID_REPORT_PROTOCOL 1
#define UHID_INPUT_REPORT 1
#define UHID_OUTPUT_REPORT 2
#define UHID_FEATURE_REPORT 3

struct usb_hid_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bcdHID;
  uint8_t bCountryCode;
  uint8_t bNumDescriptors;
  uint8_t bReportDescriptorType;
  uint16_t wReportDescriptorLength;
} __packed;

#endif
