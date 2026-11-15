#ifndef PRONINX_HID_H
#define PRONINX_HID_H

#include "inc/types.h"

// HID

enum hid_kind {
  hid_input,
  hid_output,
  hid_feature,
  hid_collection,
  hid_endcollection,
  hid_all
};

struct hid_location {
  uint32_t size;
  uint32_t count;
  uint32_t pos;
};

struct hid_item {
  uint32_t usage_page; // in the top half
  int32_t logical_minimum;
  int32_t logical_maximum;
  int32_t physical_minimum;
  int32_t physical_maximum;
  uint32_t report_ID;
  uint32_t usage; // with its page
  uint32_t collection;
  int collevel;
  enum hid_kind kind;
  uint32_t flags;
  struct hid_location loc;
};

struct hid_data;

struct hid_data *hid_start_parse(const void *desc, int len, enum hid_kind kind);
void hid_end_parse(struct hid_data *d);
int hid_get_item(struct hid_data *d, struct hid_item *item);
int hid_report_size(const void *desc, int len, enum hid_kind kind, uint8_t id);
int hid_locate(const void *desc, int len, uint32_t usage, uint8_t id,
               enum hid_kind kind, struct hid_location *loc, uint32_t *flags);
int hid_is_collection(const void *desc, int len, uint8_t id, int32_t usage);
int32_t hid_get_data(const uint8_t *report, int len, const struct hid_location *loc);
uint32_t hid_get_udata(const uint8_t *report, int len, const struct hid_location *loc);
void hid_init(void);

#define HID_USAGE2(page, usage) (((page) << 16) | (usage))
#define HID_GET_USAGE(u) ((u) & 0xffff)
#define HID_GET_USAGE_PAGE(u) (((u) >> 16) & 0xffff)

// pages
#define HUP_UNDEFINED 0x0000
#define HUP_GENERIC_DESKTOP 0x0001
#define HUP_SIMULATION 0x0002
#define HUP_VR_CONTROLS 0x0003
#define HUP_SPORTS_CONTROLS 0x0004
#define HUP_GAMING_CONTROLS 0x0005
#define HUP_KEYBOARD 0x0007
#define HUP_LED 0x0008
#define HUP_BUTTON 0x0009
#define HUP_ORDINALS 0x000a
#define HUP_TELEPHONY 0x000b
#define HUP_CONSUMER 0x000c
#define HUP_DIGITIZERS 0x000d
#define HUP_VENDOR 0x00ff

// the generic desktop
#define HUG_POINTER 0x0001
#define HUG_MOUSE 0x0002
#define HUG_JOYSTICK 0x0004
#define HUG_GAME_PAD 0x0005
#define HUG_KEYBOARD 0x0006
#define HUG_KEYPAD 0x0007
#define HUG_X 0x0030
#define HUG_Y 0x0031
#define HUG_Z 0x0032
#define HUG_RX 0x0033
#define HUG_RY 0x0034
#define HUG_RZ 0x0035
#define HUG_SLIDER 0x0036
#define HUG_DIAL 0x0037
#define HUG_WHEEL 0x0038
#define HUG_HAT_SWITCH 0x0039
#define HUG_TWHEEL 0x0048

// digitizers
#define HUD_PEN 0x0002

// LEDs
#define HUL_NUM_LOCK 0x0001
#define HUL_CAPS_LOCK 0x0002
#define HUL_SCROLL_LOCK 0x0003
#define HUL_COMPOSE 0x0004
#define HUL_KANA 0x0005

// the consumer page
#define HUC_AC_PAN 0x0238

// collections
#define HCOLL_PHYSICAL 0
#define HCOLL_APPLICATION 1
#define HCOLL_LOGICAL 2

#define HIO_CONST 0x001
#define HIO_VARIABLE 0x002
#define HIO_RELATIVE 0x004
#define HIO_WRAP 0x008
#define HIO_NONLINEAR 0x010
#define HIO_NOPREF 0x020
#define HIO_NULLSTATE 0x040
#define HIO_VOLATILE 0x080
#define HIO_BUFBYTES 0x100

#endif
