#ifndef PRONINX_ATKBDCREG_H
#define PRONINX_ATKBDCREG_H

#include "inc/types.h"

#define KBD_STATUS_PORT 0x64
#define KBD_DATA_PORT 0x60
#define KBD_COMMAND_PORT 0x64

// controller commands (sent to KBD_COMMAND_PORT)
#define KBDC_SET_COMMAND_BYTE 0x0060
#define KBDC_GET_COMMAND_BYTE 0x0020
#define KBDC_WRITE_TO_AUX_MUX 0x0090
#define KBDC_WRITE_TO_AUX 0x00d4
#define KBDC_DISABLE_AUX_PORT 0x00a7
#define KBDC_ENABLE_AUX_PORT 0x00a8
#define KBDC_TEST_AUX_PORT 0x00a9
#define KBDC_DIAGNOSE 0x00aa
#define KBDC_TEST_KBD_PORT 0x00ab
#define KBDC_DISABLE_KBD_PORT 0x00ad
#define KBDC_ENABLE_KBD_PORT 0x00ae
#define KBDC_FORCE_AUX_OUTPUT 0x00d3

#define KBD_TRANSLATION 0x0040
#define KBD_RESERVED_BITS 0x0004
#define KBD_OVERRIDE_KBD_LOCK 0x0008
#define KBD_ENABLE_KBD_PORT 0x0000
#define KBD_DISABLE_KBD_PORT 0x0010
#define KBD_ENABLE_AUX_PORT 0x0000
#define KBD_DISABLE_AUX_PORT 0x0020
#define KBD_ENABLE_AUX_INT 0x0002
#define KBD_DISABLE_AUX_INT 0x0000
#define KBD_ENABLE_KBD_INT 0x0001
#define KBD_DISABLE_KBD_INT 0x0000
#define KBD_KBD_CONTROL_BITS (KBD_DISABLE_KBD_PORT | KBD_ENABLE_KBD_INT)
#define KBD_AUX_CONTROL_BITS (KBD_DISABLE_AUX_PORT | KBD_ENABLE_AUX_INT)

#define KBDC_RESET_KBD 0x00ff
#define KBDC_ENABLE_KBD 0x00f4
#define KBDC_DISABLE_KBD 0x00f5
#define KBDC_SET_DEFAULTS 0x00f6
#define KBDC_SEND_DEV_ID 0x00f2
#define KBDC_SET_LEDS 0x00ed
#define KBDC_ECHO 0x00ee
#define KBDC_SET_SCANCODE_SET 0x00f0
#define KBDC_SET_TYPEMATIC 0x00f3

#define PSMC_RESET_DEV 0x00ff
#define PSMC_ENABLE_DEV 0x00f4
#define PSMC_DISABLE_DEV 0x00f5
#define PSMC_SET_DEFAULTS 0x00f6
#define PSMC_SEND_DEV_ID 0x00f2
#define PSMC_SEND_DEV_STATUS 0x00e9
#define PSMC_SEND_DEV_DATA 0x00eb
#define PSMC_SET_SCALING11 0x00e6
#define PSMC_SET_SCALING21 0x00e7
#define PSMC_SET_RESOLUTION 0x00e8
#define PSMC_SET_STREAM_MODE 0x00ea
#define PSMC_SET_REMOTE_MODE 0x00f0
#define PSMC_SET_SAMPLING_RATE 0x00f3

#define PSMD_RES_LOW 0
#define PSMD_RES_MEDIUM_LOW 1
#define PSMD_RES_MEDIUM_HIGH 2
#define PSMD_RES_HIGH 3
#define PSMD_MAX_RESOLUTION PSMD_RES_HIGH

// status bits (KBD_STATUS_PORT)
#define KBDS_BUFFER_FULL 0x0021
#define KBDS_ANY_BUFFER_FULL 0x0001
#define KBDS_KBD_BUFFER_FULL 0x0001
#define KBDS_AUX_BUFFER_FULL 0x0021
#define KBDS_INPUT_BUFFER_FULL 0x0002

#define KBD_ACK 0x00fa
#define KBD_RESEND 0x00fe
#define KBD_RESET_DONE 0x00aa
#define KBD_RESET_FAIL 0x00fc
#define KBD_DIAG_DONE 0x0055
#define KBD_DIAG_FAIL 0x00fd
#define KBD_ECHO 0x00ee

#define PSM_ACK 0x00fa
#define PSM_RESEND 0x00fe
#define PSM_RESET_DONE 0x00aa
#define PSM_RESET_FAIL 0x00fc

#define PSM_MOUSE_ID 0
#define PSM_BALLPOINT_ID 2
#define PSM_INTELLI_ID 3
#define PSM_EXPLORER_ID 4
#define PSM_4DMOUSE_ID 6
#define PSM_4DPLUS_ID 8
#define PSM_4DPLUS_RFSW35_ID 24

#define KBD_MAXRETRY 3
#define KBD_RESETDELAY 200
#define KBD_MAXWAIT 5
#define KBDC_DELAYTIME 20
#define KBDD_DELAYTIME 7

#define KBDC_AUX_MUX_NUM_PORTS 4

#define KBDQ_BUFSIZE 32

struct kqueue {
  int head;
  int tail;
  uchar q[KBDQ_BUFSIZE];
};

struct atkbdc_softc {
  int lock;
  void *owner;
  struct kqueue kbd;
  struct kqueue aux;
  int command_byte;
  int command_mask;
  int retry;
  int aux_mux_enabled;
  int aux_mux_port;
};

typedef struct atkbdc_softc *KBDC;

KBDC atkbdc_open(void);
int kbdc_lock(KBDC p, int lock);
int kbdc_busy(KBDC p);
int kbdc_data_ready(KBDC p);
int write_controller_command(KBDC p, int c);
int write_controller_data(KBDC p, int c);
int write_kbd_command(KBDC p, int c);
int write_aux_command(KBDC p, int c);
int send_kbd_command(KBDC p, int c);
int send_aux_command(KBDC p, int c);
int send_kbd_command_and_data(KBDC p, int c, int d);
int send_aux_command_and_data(KBDC p, int c, int d);
int read_controller_data(KBDC p);
int read_kbd_data(KBDC p);
int read_kbd_data_no_wait(KBDC p);
int read_aux_data(KBDC p);
int read_aux_data_no_wait(KBDC p);
void empty_kbd_buffer(KBDC p, int wait);
void empty_aux_buffer(KBDC p, int wait);
void empty_both_buffers(KBDC p, int wait);
int reset_kbd(KBDC p);
int reset_aux_dev(KBDC p);
int test_controller(KBDC p);
int test_kbd_port(KBDC p);
int test_aux_port(KBDC p);
int kbdc_get_device_mask(KBDC p);
void kbdc_set_device_mask(KBDC p, int mask);
int get_controller_command_byte(KBDC p);
int set_controller_command_byte(KBDC p, int mask, int command);
int set_active_aux_mux_port(KBDC p, int port);
int enable_aux_mux(KBDC p);
int disable_aux_mux(KBDC p);
int aux_mux_is_enabled(KBDC p);

#endif
