// PSM
#include "acpi/acpi.h"
#include "atkbdcreg.h"
#include "callout.h"
#include "kbdreg.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "fs.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "task.h"
#include "uio.h"
#include "x86.h"

#define PSM_INPUT_TIMEOUT 2000000
#define PSM_TAP_TIMEOUT 125000
#define PSM_TAP_THRESHOLD 25

#define PSM_LEVEL_BASE FNU_MOUSE_LEVEL_BASE
#define PSM_LEVEL_STANDARD FNU_MOUSE_LEVEL_STANDARD
#define PSM_LEVEL_NATIVE FNU_MOUSE_LEVEL_NATIVE
#define PSM_LEVEL_MIN PSM_LEVEL_BASE
#define PSM_LEVEL_MAX PSM_LEVEL_NATIVE

#define PSM_NOMUX (-1)

#define MOUSE_PS2PLUS_CHECKBITS(b) ((((b[2] & 0x03) << 2) | 0x02) == (b[1] & 0x0f))
#define MOUSE_PS2PLUS_PACKET_TYPE(b) (((b[0] & 0x30) >> 2) | ((b[1] & 0x30) >> 4))




// PS/2
#define MOUSE_PS2PLUS_SYNCMASK 0x48
#define MOUSE_PS2PLUS_SYNC 0x48
#define MOUSE_PS2PLUS_ZNEG 0x08
#define MOUSE_PS2PLUS_BUTTON4DOWN 0x10
#define MOUSE_PS2PLUS_BUTTON5DOWN 0x20
#define MOUSE_SPOINT_ZNEG 0x80
#define MOUSE_SPOINT_WNEG 0x08
#define MOUSE_PS2INTELLI_PACKETSIZE 4
#define MOUSE_PS2INTELLI_BUTTON4DOWN 0x40
#define MOUSE_PS2INTELLI_BUTTON5DOWN 0x80
#define MOUSE_EXPLORER_ZNEG 0x08
#define MOUSE_EXPLORER_BUTTON4DOWN 0x10
#define MOUSE_EXPLORER_BUTTON5DOWN 0x20
#define MOUSE_PS2VERSA_PACKETSIZE 6
#define MOUSE_PS2VERSA_IN_USE 0x10
#define MOUSE_PS2VERSA_BUTTONS 0x05
#define MOUSE_PS2VERSA_TAP 0x02
#define MOUSE_4D_PACKETSIZE 3
#define MOUSE_4D_WHEELBITS 0xf0
#define MOUSE_4DPLUS_PACKETSIZE 3
#define MOUSE_4DPLUS_ZNEG 0x04
#define MOUSE_4DPLUS_BUTTON4DOWN 0x08
#define MOUSE_SYNAPTICS_PACKETSIZE 6
#define MOUSE_ELANTECH_PACKETSIZE 6

typedef struct packetbuf {
  uchar ipacket[16];
  int inputbytes;
} packetbuf_t;

#define PSM_PACKETQUEUE 128
#define PSM_BUFSIZE 960
#define PSM_SMALLBUFSIZE 240

#define SYNAPTICS_READ_IDENTITY 0x00
#define SYNAPTICS_READ_MODES 0x01
#define SYNAPTICS_READ_CAPABILITIES 0x02
#define SYNAPTICS_READ_MODEL_ID 0x03
#define SYNAPTICS_READ_SERIAL_PREFIX 0x06
#define SYNAPTICS_READ_SERIAL_SUFFIX 0x07
#define SYNAPTICS_READ_RESOLUTIONS 0x08
#define SYNAPTICS_READ_EXTENDED 0x09
#define SYNAPTICS_READ_CAPABILITIES_CONT 0x0c
#define SYNAPTICS_READ_MAX_COORDS 0x0d
#define SYNAPTICS_READ_DELUXE_LED 0x0e
#define SYNAPTICS_READ_MIN_COORDS 0x0f

typedef struct synapticsinfo {
  int directional_scrolls, two_finger_scroll, min_pressure, max_pressure;
  int max_width, margin_top, margin_right, margin_bottom, margin_left;
  int na_top, na_right, na_bottom, na_left, window_min, window_max;
  int multiplicator, weight_current, weight_previous, weight_previous_na;
  int weight_len_squared, div_min, div_max, div_max_na, div_len;
  int tap_max_delta, tap_min_queue, taphold_timeout, vscroll_ver_area;
  int vscroll_hor_area, vscroll_min_delta, vscroll_div_min, vscroll_div_max;
  int touchpad_off, softbuttons_y, softbutton2_x, softbutton3_x, max_x, max_y;
  int three_finger_drag, natural_scroll;
} synapticsinfo_t;

typedef struct synapticspacket {
  int x;
  int y;
} synapticspacket_t;

#define SYNAPTICS_PACKETQUEUE 10
#define SYNAPTICS_QUEUE_CURSOR(x) (((x) + SYNAPTICS_PACKETQUEUE) % SYNAPTICS_PACKETQUEUE)

#define SYNAPTICS_VERSION_GE(synhw, major, minor)                              \
  ((synhw).infoMajor > (major) ||                                              \
   ((synhw).infoMajor == (major) && (synhw).infoMinor >= (minor)))

typedef struct smoother {
  synapticspacket_t queue[SYNAPTICS_PACKETQUEUE];
  int queue_len, queue_cursor, start_x, start_y, avg_dx, avg_dy;
  int squelch_x, squelch_y, is_fuzzy, active;
} smoother_t;

typedef struct gesture {
  int window_min, fingers_nb, tap_button, in_taphold, in_vscroll, zmax;
  int64_t taptimeout;
} gesture_t;

typedef struct trackpointinfo {
  int vendor, firmware, sensitivity, inertia, uplateau, reach, draghys;
  int mindrag, upthresh, threshold, jenks, ztime, pts, skipback;
} trackpointinfo_t;

#define TRACKPOINT_VENDOR_IBM 0x01
#define TRACKPOINT_VENDOR_UNKNOWN 0x07

typedef struct finger {
  int x, y, p, w, flags;
} finger_t;
#define PSM_FINGERS 2
#define PSM_FINGER_IS_PEN (1 << 0)
#define PSM_FINGER_FUZZY (1 << 1)
#define PSM_FINGER_DEFAULT_P tap_threshold
#define PSM_FINGER_DEFAULT_W 1
#define PSM_FINGER_IS_SET(f) ((f).x != -1 && (f).y != -1 && (f).p != 0)
#define PSM_FINGER_RESET(f)                                                    \
  do {                                                                         \
    (f) = (finger_t){.x = -1, .y = -1, .p = 0, .w = 0, .flags = 0};            \
  } while (0)

typedef struct elantechhw {
  int hwversion, fwversion, sizex, sizey, dpmmx, dpmmy, ntracesx, ntracesy;
  int dptracex, dptracey, issemimt, isclickpad, hassmbusnotify, has3buttons;
  int hascrc, hastrackpoint, haspressure;
} elantechhw_t;

#define ELANTECH_HW_IS_V1(fwver) ((fwver) < 0x020030 || (fwver) == 0x020600)
#define ELANTECH_MAGIC(magic)                                                  \
  ((magic)[0] == 0x3c && (magic)[1] == 0x03 &&                                 \
   ((magic)[2] == 0xc8 || (magic)[2] == 0x00))

#define ELANTECH_FW_ID 0x00
#define ELANTECH_FW_VERSION 0x01
#define ELANTECH_CAPABILITIES 0x02
#define ELANTECH_SAMPLE 0x03
#define ELANTECH_RESOLUTION 0x04
#define ELANTECH_REG_READ 0x10
#define ELANTECH_REG_WRITE 0x11
#define ELANTECH_REG_RDWR 0x00
#define ELANTECH_CUSTOM_CMD 0xf8

#define ELANTECH_MAX_FINGERS PSM_FINGERS

#define ELANTECH_FINGER_MAX_P 255
#define ELANTECH_FINGER_MAX_W 15
#define ELANTECH_FINGER_SET_XYP(pb)                                            \
  (finger_t) {                                                                 \
    .x = (((pb)->ipacket[1] & 0x0f) << 8) | (pb)->ipacket[2],                  \
    .y = (((pb)->ipacket[4] & 0x0f) << 8) | (pb)->ipacket[5],                  \
    .p = ((pb)->ipacket[1] & 0xf0) | (((pb)->ipacket[4] >> 4) & 0x0f),         \
    .w = PSM_FINGER_DEFAULT_W, .flags = 0                                      \
  }

enum {
  ELANTECH_PKT_NOP,
  ELANTECH_PKT_TRACKPOINT,
  ELANTECH_PKT_V2_COMMON,
  ELANTECH_PKT_V2_2FINGER,
  ELANTECH_PKT_V3,
  ELANTECH_PKT_V4_STATUS,
  ELANTECH_PKT_V4_HEAD,
  ELANTECH_PKT_V4_MOTION
};

#define ELANTECH_PKT_IS_TRACKPOINT(pb) (((pb)->ipacket[3] & 0x0f) == 0x06)
#define ELANTECH_PKT_IS_DEBOUNCE(pb, hwversion)                                \
  ((hwversion) == 4 ? 0                                                        \
                    : (pb)->ipacket[0] == ((hwversion) == 2 ? 0x84 : 0xc4) &&  \
                          (pb)->ipacket[1] == 0xff && (pb)->ipacket[2] == 0xff && \
                          (pb)->ipacket[3] == 0x02 && (pb)->ipacket[4] == 0xff && \
                          (pb)->ipacket[5] == 0xff)
#define ELANTECH_PKT_IS_V2(pb)                                                 \
  (((pb)->ipacket[0] & 0x0c) == 0x04 && ((pb)->ipacket[3] & 0x0f) == 0x02)
#define ELANTECH_PKT_IS_V3_HEAD(pb, hascrc)                                    \
  ((hascrc) ? ((pb)->ipacket[3] & 0x09) == 0x08                                \
            : ((pb)->ipacket[0] & 0x0c) == 0x04 && ((pb)->ipacket[3] & 0xcf) == 0x02)
#define ELANTECH_PKT_IS_V3_TAIL(pb, hascrc)                                    \
  ((hascrc) ? ((pb)->ipacket[3] & 0x09) == 0x09                                \
            : ((pb)->ipacket[0] & 0x0c) == 0x0c && ((pb)->ipacket[3] & 0xce) == 0x0c)
#define ELANTECH_PKT_IS_V4(pb, hascrc)                                         \
  ((hascrc) ? ((pb)->ipacket[3] & 0x08) == 0x00                                \
            : ((pb)->ipacket[0] & 0x08) == 0x00 && ((pb)->ipacket[3] & 0x1c) == 0x10)

typedef struct elantechaction {
  finger_t fingers[ELANTECH_MAX_FINGERS];
  int mask;
  int mask_v4wait;
} elantechaction_t;

struct psm_softc {
  struct selinfo rsel;
  uchar state;
  int config;
  int flags;
  KBDC kbdc;
  struct fnu_mousehw hw;
  struct fnu_synapticshw synhw;
  synapticsinfo_t syninfo;
  smoother_t smoother[PSM_FINGERS];
  gesture_t gesture;
  elantechhw_t elanhw;
  elantechaction_t elanaction;
  trackpointinfo_t tpinfo;
  struct fnu_mousemode mode;
  struct fnu_mousemode dflt_mode;
  struct fnu_mousestatus status;
  struct {
    int count, head, tail;
    uchar buf[PSM_BUFSIZE];
  } queue; // /dev/psm
  packetbuf_t pqueue[PSM_PACKETQUEUE];
  int pqueue_start;
  int pqueue_end;
  int button;
  int xold;
  int yold;
  int syncerrors;
  int pkterrors;
  int fpcount;
  int64_t inputtimeout;
  int64_t lastsoftintr;
  int64_t lastinputerr;
  int64_t idletimeout;
  packetbuf_t idlepacket;
  int watchdog;
  struct callout callout;     // the watchdog
  struct callout softcallout;
  int lasterr;
  int cmdcount;
  int extended_buttons;
  int muxport;
  uchar muxsave[3];
  int muxtpbuttons;
  int muxmsbuttons;
  int64_t muxmidtimeout;
  int muxsinglesyna;
  int unit;
  int syninfo_set;
  int tptree;
  struct task recover; // what psmintr cannot do itself
  int recover_reset;
};

#define PSM_VALID 0x80
#define PSM_OPEN 1
#define PSM_ASLP 2
#define PSM_SOFTARMED 4
#define PSM_NEED_SYNCBITS 8
#define PSM_DEVOPEN 0x10

#define PSM_CONFIG_RESOLUTION 0x000f
#define PSM_CONFIG_ACCEL 0x00f0
#define PSM_CONFIG_NOCHECKSYNC 0x0100
#define PSM_CONFIG_NOIDPROBE 0x0200
#define PSM_CONFIG_NORESET 0x0400
#define PSM_CONFIG_FORCETAP 0x0800
#define PSM_CONFIG_IGNPORTERROR 0x1000

#define PSM_FLAGS_FINGERDOWN 0x0001

// hw.psm.* and debug.psm.*
static int tap_enabled = -1;
static int verbose;
static int synaptics_support = 1;
static int trackpoint_support = 1;
static int elantech_support = 1;
static int mux_disabled = -1;
static int psmhz = 20;
static int psmerrsecs = 2;
static int psmerrusecs = 0;
static int psmsecs = 0;
static int psmusecs = 500000;
static int pkterrthresh = 2;
static int tap_threshold = PSM_TAP_THRESHOLD;
static int tap_timeout = PSM_TAP_TIMEOUT;

static struct psm_softc psm_sc;

// PSMCPNP
enum { PSMCPNP_NONE, PSMCPNP_GENERIC, PSMCPNP_FORCEPAD, PSMCPNP_TOPBUTTONPAD };
static int pnp_type;

static const char *const pnp_forcepad[] = {"SYN300D", "SYN3014", 0};
static const char *const pnp_topbutton[] = {
    "LEN0017", "LEN0018", "LEN0019", "LEN0023", "LEN002A", "LEN002B", "LEN002C",
    "LEN002D", "LEN002E", "LEN0033", "LEN0034", "LEN0035", "LEN0036", "LEN0037",
    "LEN0038", "LEN0039", "LEN0041", "LEN0042", "LEN0045", "LEN0047", "LEN0049",
    "LEN2000", "LEN2001", "LEN2002", "LEN2003", "LEN2004", "LEN2005", "LEN2006",
    "LEN2007", "LEN2008", "LEN2009", "LEN200A", "LEN200B", 0};
static const char *const pnp_ids[] = {
    "SYN300D", "SYN3014", "LEN0017", "LEN0018", "LEN0019", "LEN0023", "LEN002A",
    "LEN002B", "LEN002C", "LEN002D", "LEN002E", "LEN0033", "LEN0034", "LEN0035",
    "LEN0036", "LEN0037", "LEN0038", "LEN0039", "LEN0041", "LEN0042", "LEN0045",
    "LEN0047", "LEN0049", "LEN2000", "LEN2001", "LEN2002", "LEN2003", "LEN2004",
    "LEN2005", "LEN2006", "LEN2007", "LEN2008", "LEN2009", "LEN200A", "LEN200B",
    "PNP0F03", "PNP0F0E", "PNP0F12", "PNP0F13", "PNP0313", "SYN0002", "AUI0200",
    "IBM3780", "IBM3781", "SNY9001", "SNY9002", "SNY9003", "SNY9004", 0};

static int pnp_in(const char *id, const char *const *list) {
  for (; *list != 0; list++)
    if (strcmp(id, *list) == 0)
      return 1;
  return 0;
}

static void psmcpnp_attach(struct aml_node *dev, const char *hid) {
  int t = pnp_in(hid, pnp_forcepad)    ? PSMCPNP_FORCEPAD
          : pnp_in(hid, pnp_topbutton) ? PSMCPNP_TOPBUTTONPAD
                                       : PSMCPNP_GENERIC;

  (void)dev;
  if (t > pnp_type)
    pnp_type = t;
  if (verbose >= 1)
    printf("psmcpnp: %s\n", hid);
}

const struct acpi_driver psm_acpi_driver = {"psmcpnp", pnp_ids, 0, psmcpnp_attach};

enum probearg { PROBE, REINIT };
typedef int probefunc_t(struct psm_softc *, enum probearg);

static int mouse_id_proc1(KBDC, int, int, int *);
static int mouse_ext_command(KBDC, int);

static probefunc_t enable_groller;
static probefunc_t enable_gmouse;
static probefunc_t enable_aglide;
static probefunc_t enable_kmouse;
static probefunc_t enable_msexplorer;
static probefunc_t enable_msintelli;
static probefunc_t enable_4dmouse;
static probefunc_t enable_4dplus;
static probefunc_t enable_mmanplus;
static probefunc_t enable_synaptics;
static probefunc_t enable_synaptics_mux;
static probefunc_t enable_single_synaptics_mux;
static probefunc_t enable_trackpoint;
static probefunc_t enable_versapad;
static probefunc_t enable_elantech;

static void set_trackpoint_parameters(struct psm_softc *sc, int por);
static void synaptics_passthrough_on(struct psm_softc *sc);
static void synaptics_passthrough_off(struct psm_softc *sc);
static int synaptics_preferred_mode(struct psm_softc *sc);
static void synaptics_set_mode(struct psm_softc *sc, int mode_byte);
static void psmsoftintr(struct psm_softc *sc);
static void psmintr(struct psm_softc *sc);
static void psmsoftintr_callout(void *arg);
static void psmsoftintridle_callout(void *arg);
static void psmtimeout(void *arg);
static int reinitialize(struct psm_softc *sc, int doinit);
static void synaptics_sysctl_create_tree(struct psm_softc *sc);
static void trackpoint_sysctl_create_tree(struct psm_softc *sc);
static void synaptics_defaults(struct psm_softc *sc);
static void trackpoint_defaults(struct psm_softc *sc);

// the order of probing matters
static struct {
  int model;
  uchar syncmask;
  int packetsize;
  probefunc_t *probefunc;
} vendortype[] = {
    {FNU_MOUSE_MODEL_SYNAPTICS, 0x00, FNU_MOUSE_PS2_PACKETSIZE, enable_synaptics_mux},
    {FNU_MOUSE_MODEL_SYNAPTICS, 0xc0, MOUSE_SYNAPTICS_PACKETSIZE, enable_single_synaptics_mux},
    {FNU_MOUSE_MODEL_NET, 0x08, MOUSE_PS2INTELLI_PACKETSIZE, enable_gmouse},
    {FNU_MOUSE_MODEL_NETSCROLL, 0xc8, 6, enable_groller},
    {FNU_MOUSE_MODEL_MOUSEMANPLUS, 0x08, FNU_MOUSE_PS2_PACKETSIZE, enable_mmanplus},
    {FNU_MOUSE_MODEL_EXPLORER, 0x08, MOUSE_PS2INTELLI_PACKETSIZE, enable_msexplorer},
    {FNU_MOUSE_MODEL_4D, 0x08, MOUSE_4D_PACKETSIZE, enable_4dmouse},
    {FNU_MOUSE_MODEL_4DPLUS, 0xc8, MOUSE_4DPLUS_PACKETSIZE, enable_4dplus},
    {FNU_MOUSE_MODEL_SYNAPTICS, 0xc0, MOUSE_SYNAPTICS_PACKETSIZE, enable_synaptics},
    {FNU_MOUSE_MODEL_ELANTECH, 0x04, MOUSE_ELANTECH_PACKETSIZE, enable_elantech},
    {FNU_MOUSE_MODEL_INTELLI, 0x08, MOUSE_PS2INTELLI_PACKETSIZE, enable_msintelli},
    {FNU_MOUSE_MODEL_GLIDEPOINT, 0xc0, FNU_MOUSE_PS2_PACKETSIZE, enable_aglide},
    {FNU_MOUSE_MODEL_THINK, 0x80, FNU_MOUSE_PS2_PACKETSIZE, enable_kmouse},
    {FNU_MOUSE_MODEL_VERSAPAD, 0xe8, MOUSE_PS2VERSA_PACKETSIZE, enable_versapad},
    {FNU_MOUSE_MODEL_TRACKPOINT, 0xc0, FNU_MOUSE_PS2_PACKETSIZE, enable_trackpoint},
    {FNU_MOUSE_MODEL_GENERIC, 0xc0, FNU_MOUSE_PS2_PACKETSIZE, 0},
};
#define GENERIC_MOUSE_ENTRY ((int)(sizeof(vendortype) / sizeof(vendortype[0])) - 1)

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

static int bitcount(int v) {
  int n = 0;

  for (; v != 0; v &= v - 1)
    n++;
  return n;
}

static int64_t now_us(void) {
  uint64_t hz = delay_tsc_hz();

  return hz ? (int64_t)(rdtsc() / (hz / 1000000)) : (int64_t)ticks * 10000;
}

static int timeelapsed(int64_t start, int secs, int usecs, int64_t now) {
  return start + (int64_t)secs * 1000000 + usecs < now;
}

// AUX DEVICE COMMANDS

static int enable_aux_dev(KBDC kbdc) {
  int res = send_aux_command(kbdc, PSMC_ENABLE_DEV);

  if (verbose >= 2)
    printf("psm: ENABLE_DEV return code:%x\n", res);
  return res == PSM_ACK;
}

static int disable_aux_dev(KBDC kbdc) {
  int res = send_aux_command(kbdc, PSMC_DISABLE_DEV);

  if (verbose >= 2)
    printf("psm: DISABLE_DEV return code:%x\n", res);
  return res == PSM_ACK;
}

static int get_mouse_status(KBDC kbdc, int *status, int flag, int len) {
  int cmd = flag == 1 ? PSMC_SEND_DEV_DATA : PSMC_SEND_DEV_STATUS, res, i;

  empty_aux_buffer(kbdc, 5);
  res = send_aux_command(kbdc, cmd);
  if (verbose >= 2)
    printf("psm: SEND_AUX_DEV_%s return code:%x\n", flag == 1 ? "DATA" : "STATUS", res);
  if (res != PSM_ACK)
    return 0;

  for (i = 0; i < len; ++i) {
    status[i] = read_aux_data(kbdc);
    if (status[i] < 0)
      break;
  }
  if (len >= 3) {
    for (; i < 3; ++i)
      status[i] = 0;
    if (verbose >= 1)
      printf("psm: %s %x %x %x\n", flag == 1 ? "data" : "status", status[0],
              status[1], status[2]);
  }
  return i;
}

static int get_aux_id(KBDC kbdc) {
  int res, id;

  empty_aux_buffer(kbdc, 5);
  res = send_aux_command(kbdc, PSMC_SEND_DEV_ID);
  if (verbose >= 2)
    printf("psm: SEND_DEV_ID return code:%x\n", res);
  if (res != PSM_ACK)
    return -1;
  delay(10000);
  id = read_aux_data(kbdc);
  if (verbose >= 2)
    printf("psm: device ID: %x\n", id);
  return id;
}

static int set_mouse_sampling_rate(KBDC kbdc, int rate) {
  int res = send_aux_command_and_data(kbdc, PSMC_SET_SAMPLING_RATE, rate);

  if (verbose >= 2)
    printf("psm: SET_SAMPLING_RATE (%d) %x\n", rate, res);
  return res == PSM_ACK ? rate : -1;
}

static int set_mouse_scaling(KBDC kbdc, int scale) {
  int res;

  scale = scale == 2 ? PSMC_SET_SCALING21 : PSMC_SET_SCALING11;
  res = send_aux_command(kbdc, scale);
  if (verbose >= 2)
    printf("psm: SET_SCALING%s return code:%x\n",
            scale == PSMC_SET_SCALING21 ? "21" : "11", res);
  return res == PSM_ACK;
}

static int set_mouse_resolution(KBDC kbdc, int val) {
  int res = send_aux_command_and_data(kbdc, PSMC_SET_RESOLUTION, val);

  if (verbose >= 2)
    printf("psm: SET_RESOLUTION (%d) %x\n", val, res);
  return res == PSM_ACK ? val : -1;
}

static int set_mouse_mode(KBDC kbdc) {
  int res = send_aux_command(kbdc, PSMC_SET_STREAM_MODE);

  if (verbose >= 2)
    printf("psm: SET_STREAM_MODE return code:%x\n", res);
  return res == PSM_ACK;
}

static int get_mouse_buttons(KBDC kbdc) {
  int c = 2, status[3];

  if (set_mouse_resolution(kbdc, PSMD_RES_LOW) != PSMD_RES_LOW)
    return c;
  if (set_mouse_scaling(kbdc, 1) && set_mouse_scaling(kbdc, 1) &&
      set_mouse_scaling(kbdc, 1) && get_mouse_status(kbdc, status, 0, 3) >= 3 &&
      status[1] != 0)
    return status[1];
  return c;
}

static int is_a_mouse(int id __unused) { return 1; }

static const char *model_name(int model) {
  static const struct {
    int model_code;
    const char *model_name;
  } models[] = {
      {FNU_MOUSE_MODEL_NETSCROLL, "NetScroll"},
      {FNU_MOUSE_MODEL_NET, "NetMouse/NetScroll Optical"},
      {FNU_MOUSE_MODEL_GLIDEPOINT, "GlidePoint"},
      {FNU_MOUSE_MODEL_THINK, "ThinkingMouse"},
      {FNU_MOUSE_MODEL_INTELLI, "IntelliMouse"},
      {FNU_MOUSE_MODEL_MOUSEMANPLUS, "MouseMan+"},
      {FNU_MOUSE_MODEL_VERSAPAD, "VersaPad"},
      {FNU_MOUSE_MODEL_EXPLORER, "IntelliMouse Explorer"},
      {FNU_MOUSE_MODEL_4D, "4D Mouse"},
      {FNU_MOUSE_MODEL_4DPLUS, "4D+ Mouse"},
      {FNU_MOUSE_MODEL_SYNAPTICS, "Synaptics Touchpad"},
      {FNU_MOUSE_MODEL_TRACKPOINT, "IBM/Lenovo TrackPoint"},
      {FNU_MOUSE_MODEL_ELANTECH, "Elantech Touchpad"},
      {FNU_MOUSE_MODEL_GENERIC, "Generic PS/2 mouse"},
      {FNU_MOUSE_MODEL_UNKNOWN, "Unknown"},
  };
  int i;

  for (i = 0; models[i].model_code != FNU_MOUSE_MODEL_UNKNOWN; ++i)
    if (models[i].model_code == model)
      break;
  return models[i].model_name;
}

static void recover_from_error(KBDC kbdc) {
  empty_both_buffers(kbdc, 10);
  if (!test_controller(kbdc))
    printf("psm: keyboard controller failed.\n");
  if (test_kbd_port(kbdc) != 0 && verbose >= 1)
    printf("psm: keyboard port failed.\n");
}

static int restore_controller(KBDC kbdc, int command_byte) {
  empty_both_buffers(kbdc, 10);
  if (!set_controller_command_byte(kbdc, 0xff, command_byte)) {
    printf("psm: failed to restore the keyboard controller command byte.\n");
    empty_both_buffers(kbdc, 10);
    return 0;
  }
  empty_both_buffers(kbdc, 10);
  return 1;
}

static void dropqueue(struct psm_softc *sc) {
  sc->queue.count = sc->queue.head = sc->queue.tail = 0;
  if ((sc->state & PSM_SOFTARMED) != 0) {
    sc->state &= ~PSM_SOFTARMED;
    callout_stop_async(&sc->softcallout);
  }
  sc->pqueue_start = sc->pqueue_end;
}

static void flushpackets(struct psm_softc *sc) {
  dropqueue(sc);
  memset(&sc->pqueue, 0, sizeof(sc->pqueue));
}

static int doinitialize(struct psm_softc *sc, struct fnu_mousemode *mode) {
  KBDC kbdc = sc->kbdc;
  int stat[3], i;

  switch ((i = test_aux_port(kbdc))) {
  case 1:
  case 2:
  case 3:
  case PSM_ACK:
    if (verbose)
      printf("psm: strange result for test aux port (%d).\n", i);
    // fallthrough
  case 0:
    break;
  case -1:
  default:
    recover_from_error(kbdc);
    if (sc->config & PSM_CONFIG_IGNPORTERROR)
      break;
    printf("psm: the aux port is not functioning (%d).\n", i);
    return 0;
  }

  if (!(sc->config & PSM_CONFIG_NORESET)) {
    if (!reset_aux_dev(kbdc)) {
      recover_from_error(kbdc);
      printf("psm: failed to reset the aux device.\n");
      return 0;
    }
  }

  if (!enable_aux_dev(kbdc) || !disable_aux_dev(kbdc)) {
    printf("psm: failed to enable the aux device.\n");
    return 0;
  }
  empty_both_buffers(kbdc, 10);

  for (i = 0; vendortype[i].probefunc != 0; ++i)
    if (vendortype[i].model == sc->hw.model)
      (*vendortype[i].probefunc)(sc, REINIT);

  if (mode != 0) {
    if (mode->rate > 0)
      mode->rate = set_mouse_sampling_rate(kbdc, mode->rate);
    if (mode->resolution >= 0)
      mode->resolution = set_mouse_resolution(kbdc, mode->resolution);
    set_mouse_scaling(kbdc, 1);
    set_mouse_mode(kbdc);
  }

  sc->flags |= PSM_NEED_SYNCBITS;

  if (get_mouse_status(kbdc, stat, 0, 3) < 3)
    printf("psm: failed to get status (doinitialize).\n");
  return 1;
}

static int doopen(struct psm_softc *sc, int command_byte) {
  int stat[3], mux_enabled = 0;

  if (sc->hw.model == FNU_MOUSE_MODEL_SYNAPTICS) {
    if (sc->muxport != PSM_NOMUX) {
      mux_enabled = enable_aux_mux(sc->kbdc) >= 0;
      if (mux_enabled)
        set_active_aux_mux_port(sc->kbdc, sc->muxport);
      else
        printf("psm: failed to enable active multiplexing mode.\n");
    }
    mouse_ext_command(sc->kbdc, SYNAPTICS_READ_MODES);
    if (get_mouse_status(sc->kbdc, stat, 0, 3) >= 3 &&
        (SYNAPTICS_VERSION_GE(sc->synhw, 7, 5) || stat[1] == 0x47) &&
        stat[2] == 0x40) {
      synaptics_set_mode(sc, synaptics_preferred_mode(sc));
      if (verbose >= 5)
        printf("psm: Synaptis Absolute Mode hopefully restored\n");
    }
    if (mux_enabled)
      disable_aux_mux(sc->kbdc);
  }

  if (sc->hw.model == FNU_MOUSE_MODEL_GENERIC) {
    if (tap_enabled > 0) {
      if (verbose >= 2)
        printf("psm: enable tap and drag gestures\n");
      synaptics_set_mode(sc, synaptics_preferred_mode(sc));
    } else if (tap_enabled == 0) {
      if (verbose >= 2)
        printf("psm: disable tap and drag gestures\n");
      synaptics_set_mode(sc, synaptics_preferred_mode(sc));
    }
  }

  if (!enable_aux_dev(sc->kbdc)) {
    recover_from_error(sc->kbdc);
    restore_controller(sc->kbdc, command_byte);
    sc->state &= ~PSM_VALID;
    printf("psm: failed to enable the device (doopen).\n");
    return -1;
  }

  if (get_mouse_status(sc->kbdc, stat, 0, 3) < 3)
    printf("psm: failed to get status (doopen).\n");

  if (!set_controller_command_byte(sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                                   (command_byte & KBD_KBD_CONTROL_BITS) |
                                       KBD_ENABLE_AUX_PORT | KBD_ENABLE_AUX_INT)) {
    disable_aux_dev(sc->kbdc);
    restore_controller(sc->kbdc, command_byte);
    printf("psm: failed to enable the aux interrupt (doopen).\n");
    return -1;
  }

  sc->watchdog = 0;
  callout_reset(&sc->callout, HZ * 2, psmtimeout, sc);
  return 0;
}

// a thread's: it owns the controller meanwhile
static int reinitialize(struct psm_softc *sc, int doinit) {
  int err = 0, c;

  if (!kbdc_lock(sc->kbdc, 1))
    return -1;

  sc->watchdog = 0;
  callout_stop_async(&sc->callout);

  empty_both_buffers(sc->kbdc, 10);
  c = get_controller_command_byte(sc->kbdc);
  if (verbose >= 2)
    printf("psm: current command byte: %x (reinitialize).\n", c);

  if (c == -1 || !set_controller_command_byte(
                     sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                     KBD_DISABLE_KBD_PORT | KBD_DISABLE_KBD_INT |
                         KBD_ENABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
    kbdc_lock(sc->kbdc, 0);
    printf("psm: unable to set the command byte (reinitialize).\n");
    return -1;
  }

  if (sc->state & PSM_VALID) {
    disable_aux_dev(sc->kbdc);
    empty_aux_buffer(sc->kbdc, 10);
  }
  mtx_enter(&kbd_lock);
  flushpackets(sc);
  mtx_leave(&kbd_lock);
  sc->syncerrors = 0;
  sc->pkterrors = 0;
  sc->lastinputerr = 0;

  if (doinit) {
    if (doinitialize(sc, &sc->mode)) {
      sc->state |= PSM_VALID;
    } else {
      restore_controller(sc->kbdc, c);
      sc->state &= ~PSM_VALID;
      printf("psm: the aux device has gone! (reinitialize).\n");
      err = -1;
    }
  }

  if ((sc->state & PSM_OPEN) && err == 0) {
    err = doopen(sc, c);
    if (err != 0)
      printf("psm: failed to enable the device (reinitialize).\n");
  } else {
    if (!set_controller_command_byte(sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                                     (c & KBD_KBD_CONTROL_BITS) |
                                         KBD_DISABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
      printf("psm: failed to disable the aux port (reinitialize).\n");
      err = -1;
    }
  }

  kbdc_lock(sc->kbdc, 0);
  return err;
}

// THE PROBE

static int psmprobe(struct psm_softc *sc) {
  int stat[3], command_byte, mask, i;

  sc->kbdc = atkbdc_open();
  sc->config = 0;
  sc->flags = 0;
  sc->muxport = PSM_NOMUX;

  if (!kbdc_lock(sc->kbdc, 1)) {
    printf("psm: unable to lock the controller.\n");
    return -1;
  }

  empty_both_buffers(sc->kbdc, 10);

  mask = kbdc_get_device_mask(sc->kbdc) & ~KBD_AUX_CONTROL_BITS;
  command_byte = get_controller_command_byte(sc->kbdc);
  if (verbose)
    printf("psm: current command byte:%x\n", command_byte);
  if (command_byte == -1) {
    printf("psm: unable to get the current command byte value.\n");
    goto fail;
  }

  if (!set_controller_command_byte(sc->kbdc,
                                   KBD_KBD_CONTROL_BITS | KBD_AUX_CONTROL_BITS,
                                   KBD_DISABLE_KBD_PORT | KBD_DISABLE_KBD_INT |
                                       KBD_ENABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
    restore_controller(sc->kbdc, command_byte);
    printf("psm: unable to set the command byte.\n");
    goto fail;
  }
  write_controller_command(sc->kbdc, KBDC_ENABLE_AUX_PORT);

  switch ((i = test_aux_port(sc->kbdc))) {
  case 1:
  case 2:
  case 3:
  case PSM_ACK:
    if (verbose)
      printf("psm: strange result for test aux port (%d).\n", i);
    // fallthrough
  case 0:
    break;
  case -1:
  default:
    recover_from_error(sc->kbdc);
    if (sc->config & PSM_CONFIG_IGNPORTERROR)
      break;
    restore_controller(sc->kbdc, command_byte);
    if (verbose)
      printf("psm: the aux port is not functioning (%d).\n", i);
    goto fail;
  }

  if (!(sc->config & PSM_CONFIG_NORESET)) {
    // twice, for KVM switches
    if (!reset_aux_dev(sc->kbdc)) {
      recover_from_error(sc->kbdc);
      restore_controller(sc->kbdc, command_byte);
      if (verbose)
        printf("psm: failed to reset the aux device.\n");
      goto fail;
    } else if (!reset_aux_dev(sc->kbdc)) {
      recover_from_error(sc->kbdc);
      if (verbose >= 2)
        printf("psm: failed to reset the aux device (2).\n");
    }
  }

  if (!enable_aux_dev(sc->kbdc) || !disable_aux_dev(sc->kbdc)) {
    recover_from_error(sc->kbdc);
    restore_controller(sc->kbdc, command_byte);
    if (verbose)
      printf("psm: failed to enable the aux device.\n");
    goto fail;
  }

  if (get_mouse_status(sc->kbdc, stat, 0, 3) >= 3) {
    sc->dflt_mode.rate = sc->mode.rate = stat[2];
    sc->dflt_mode.resolution = sc->mode.resolution = stat[1];
  } else {
    sc->dflt_mode.rate = sc->mode.rate = -1;
    sc->dflt_mode.resolution = sc->mode.resolution = -1;
  }

  sc->hw.iftype = FNU_MOUSE_IF_PS2;

  sc->hw.hwid = get_aux_id(sc->kbdc);
  if (!is_a_mouse(sc->hw.hwid)) {
    restore_controller(sc->kbdc, command_byte);
    if (verbose)
      printf("psm: unknown device type (%d).\n", sc->hw.hwid);
    goto fail;
  }
  switch (sc->hw.hwid) {
  case PSM_BALLPOINT_ID:
    sc->hw.type = FNU_MOUSE_TRACKBALL;
    break;
  case PSM_MOUSE_ID:
  case PSM_INTELLI_ID:
  case PSM_EXPLORER_ID:
  case PSM_4DMOUSE_ID:
  case PSM_4DPLUS_ID:
    sc->hw.type = FNU_MOUSE_MOUSE;
    break;
  default:
    sc->hw.type = FNU_MOUSE_UNKNOWN;
    break;
  }

  if (sc->config & PSM_CONFIG_NOIDPROBE) {
    sc->hw.buttons = 2;
    i = GENERIC_MOUSE_ENTRY;
  } else {
    sc->hw.buttons = get_mouse_buttons(sc->kbdc);
    for (i = 0; vendortype[i].probefunc != 0; ++i)
      if ((*vendortype[i].probefunc)(sc, PROBE)) {
        if (verbose >= 2)
          printf("psm: found %s\n", model_name(vendortype[i].model));
        break;
      }
  }

  sc->hw.model = vendortype[i].model;

  sc->dflt_mode.level = PSM_LEVEL_BASE;
  sc->dflt_mode.packetsize = FNU_MOUSE_PS2_PACKETSIZE;
  sc->dflt_mode.accelfactor = (sc->config & PSM_CONFIG_ACCEL) >> 4;
  if (sc->config & PSM_CONFIG_NOCHECKSYNC)
    sc->dflt_mode.syncmask[0] = 0;
  else
    sc->dflt_mode.syncmask[0] = vendortype[i].syncmask;
  if (sc->config & PSM_CONFIG_FORCETAP)
    sc->dflt_mode.syncmask[0] &= ~FNU_MOUSE_PS2_TAP;
  sc->dflt_mode.syncmask[1] = 0;
  sc->mode = sc->dflt_mode;
  sc->mode.packetsize = vendortype[i].packetsize;

  if (sc->config & PSM_CONFIG_RESOLUTION)
    sc->mode.resolution =
        set_mouse_resolution(sc->kbdc, (sc->config & PSM_CONFIG_RESOLUTION) - 1);
  else if (sc->mode.resolution >= 0)
    sc->mode.resolution = set_mouse_resolution(sc->kbdc, sc->dflt_mode.resolution);
  if (sc->mode.rate > 0)
    sc->mode.rate = set_mouse_sampling_rate(sc->kbdc, sc->dflt_mode.rate);
  set_mouse_scaling(sc->kbdc, 1);

  sc->flags |= PSM_NEED_SYNCBITS;

  if (sc->hw.model == FNU_MOUSE_MODEL_ELANTECH &&
      (sc->elanhw.hascrc || sc->elanhw.hastrackpoint)) {
    sc->config |= PSM_CONFIG_NOCHECKSYNC;
    sc->flags &= ~PSM_NEED_SYNCBITS;
  }

  if (get_mouse_status(sc->kbdc, stat, 0, 3) < 3)
    printf("psm: failed to get status.\n");
  else {
    sc->dflt_mode.rate = sc->mode.rate = stat[2];
    sc->dflt_mode.resolution = sc->mode.resolution = stat[1];
  }

  if (!set_controller_command_byte(sc->kbdc,
                                   KBD_KBD_CONTROL_BITS | KBD_AUX_CONTROL_BITS,
                                   (command_byte & KBD_KBD_CONTROL_BITS) |
                                       KBD_DISABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
    restore_controller(sc->kbdc, command_byte);
    printf("psm: unable to set the command byte.\n");
    goto fail;
  }

  kbdc_set_device_mask(sc->kbdc, mask | KBD_AUX_CONTROL_BITS);
  kbdc_lock(sc->kbdc, 0);
  return 0;

fail:
  kbdc_set_device_mask(sc->kbdc, mask);
  kbdc_lock(sc->kbdc, 0);
  return -1;
}

static int psmopen(struct psm_softc *sc) {
  int command_byte, err;

  if ((sc->state & PSM_VALID) == 0)
    return -1;

  sc->button = 0;
  memset(&sc->status, 0, sizeof(sc->status));
  flushpackets(sc);
  sc->syncerrors = 0;
  sc->pkterrors = 0;
  sc->lastinputerr = 0;

  if (!kbdc_lock(sc->kbdc, 1))
    return -1;

  empty_both_buffers(sc->kbdc, 10);
  command_byte = get_controller_command_byte(sc->kbdc);
  if (verbose >= 2)
    printf("psm: current command byte:%x (psmopen).\n", command_byte);

  if (command_byte == -1 ||
      !set_controller_command_byte(sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                                   KBD_DISABLE_KBD_PORT | KBD_DISABLE_KBD_INT |
                                       KBD_ENABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
    kbdc_lock(sc->kbdc, 0);
    printf("psm: unable to set the command byte (psmopen).\n");
    return -1;
  }

  err = doopen(sc, command_byte);
  if (err == 0)
    sc->state |= PSM_OPEN;
  kbdc_lock(sc->kbdc, 0);
  return err;
}

// THE PACKETS

static void proc_mmanplus(struct psm_softc *sc __unused, packetbuf_t *pb,
                          struct fnu_mousestatus *ms, int *x, int *y, int *z) {
  if (((pb->ipacket[0] & MOUSE_PS2PLUS_SYNCMASK) == MOUSE_PS2PLUS_SYNC) &&
      (iabs(*x) > 191) && MOUSE_PS2PLUS_CHECKBITS(pb->ipacket)) {
    switch (MOUSE_PS2PLUS_PACKET_TYPE(pb->ipacket)) {
    case 1:
      *x = *y = 0;
      if (pb->ipacket[2] & 0x80) {
        ;
      } else {
        *z = (pb->ipacket[2] & MOUSE_PS2PLUS_ZNEG) ? (pb->ipacket[2] & 0x0f) - 16
                                                   : (pb->ipacket[2] & 0x0f);
      }
      ms->button |= (pb->ipacket[2] & MOUSE_PS2PLUS_BUTTON4DOWN) ? FNU_MOUSE_BUTTON4DOWN : 0;
      ms->button |= (pb->ipacket[2] & MOUSE_PS2PLUS_BUTTON5DOWN) ? FNU_MOUSE_BUTTON5DOWN : 0;
      break;
    case 2:
      *x = *y = 0;
      if (pb->ipacket[2] & 0x0f)
        *z = (pb->ipacket[2] & MOUSE_SPOINT_WNEG) ? -2 : 2;
      if (pb->ipacket[2] & 0xf0)
        *z = (pb->ipacket[2] & MOUSE_SPOINT_ZNEG) ? -1 : 1;
      break;
    case 0:
    default:
      *x = *y = 0;
      ms->button = ms->obutton;
      if (verbose >= 1)
        printf("psmintr: unknown PS2++ packet type %d: %x %x %x\n",
                MOUSE_PS2PLUS_PACKET_TYPE(pb->ipacket), pb->ipacket[0],
                pb->ipacket[1], pb->ipacket[2]);
      break;
    }
  } else {
    ms->button |= ms->obutton & FNU_MOUSE_EXTBUTTONS;
  }
}

static int psmpalmdetect(struct psm_softc *sc, finger_t *f, int nfingers);

static uint32 ws_buttons(int button);

// what wsmouse NATIVE reports
static void psm_touch(struct psm_softc *sc, const finger_t *f, int nfingers, int buttons) {
  int y = sc->synhw.minimumYCoord + sc->synhw.maximumYCoord - f->y;

  if (nfingers > 0 && PSM_FINGER_IS_SET(*f))
    wsmouse_touch(sc->unit, f->x, y, f->p, f->w, nfingers, ws_buttons(buttons));
  else
    wsmouse_touch(sc->unit, -1, -1, 0, 0, nfingers, ws_buttons(buttons));
}
static void psmgestures(struct psm_softc *sc, finger_t *fingers, int nfingers,
                        struct fnu_mousestatus *ms);
static void psmsmoother(struct psm_softc *sc, finger_t *f, int smoother_id,
                        struct fnu_mousestatus *ms, int *x, int *y);

//   0: 1 0 W3 W2 0 W1 R L   1: Yb Ya Y9 Y8 Xb Xa X9 X8   2: Z
//   3: 1 1 Yc Xc 0 W0 D^R U^L   4: X7..X0   5: Y7..Y0
static int proc_synaptics(struct psm_softc *sc, packetbuf_t *pb,
                          struct fnu_mousestatus *ms, int *x, int *y, int *z) {
  static int touchpad_buttons;
  static int guest_buttons;
  static int ew_finger_count;
  static finger_t f[PSM_FINGERS];
  int w, id, nfingers, palm, ewcode, extended_buttons, clickpad_pressed;

  extended_buttons = 0;

  if (!synaptics_support)
    return 0;

  if ((pb->ipacket[0] & 0xc8) != 0x80 || (pb->ipacket[3] & 0xc8) != 0xc0)
    return -1;

  *x = *y = 0;
  ms->button = ms->obutton;

  *z = pb->ipacket[2];

  if (sc->synhw.capExtended)
    w = ((pb->ipacket[0] & 0x30) >> 2) | ((pb->ipacket[0] & 0x04) >> 1) |
        ((pb->ipacket[3] & 0x04) >> 2);
  else
    w = 4;

  switch (w) {
  case 3:
    if (sc->synhw.capPassthrough || sc->muxport != PSM_NOMUX) {
      *x = ((pb->ipacket[1] & 0x10) ? pb->ipacket[4] - 256 : pb->ipacket[4]);
      *y = ((pb->ipacket[1] & 0x20) ? pb->ipacket[5] - 256 : pb->ipacket[5]);
      *z = 0;

      guest_buttons = 0;
      if (pb->ipacket[1] & 0x01)
        guest_buttons |= FNU_MOUSE_BUTTON1DOWN;
      if (pb->ipacket[1] & 0x04)
        guest_buttons |= FNU_MOUSE_BUTTON2DOWN;
      if (pb->ipacket[1] & 0x02)
        guest_buttons |= FNU_MOUSE_BUTTON3DOWN;
      ms->button = touchpad_buttons | guest_buttons | sc->extended_buttons;
    }
    goto SYNAPTICS_END;

  case 2:
    ewcode = (pb->ipacket[5] & 0xf0) >> 4;
    switch (ewcode) {
    case 1:
      if (sc->synhw.capAdvancedGestures)
        f[1] = (finger_t){
            .x = (((pb->ipacket[4] & 0x0f) << 8) | pb->ipacket[1]) << 1,
            .y = (((pb->ipacket[4] & 0xf0) << 4) | pb->ipacket[2]) << 1,
            .p = ((pb->ipacket[3] & 0x30) | (pb->ipacket[5] & 0x0f)) << 1,
            .w = PSM_FINGER_DEFAULT_W,
            .flags = PSM_FINGER_FUZZY,
        };
      else if (sc->synhw.capReportsV)
        f[1] = (finger_t){
            .x = (((pb->ipacket[4] & 0x0f) << 8) | (pb->ipacket[1] & 0xfe)) << 1,
            .y = (((pb->ipacket[4] & 0xf0) << 4) | (pb->ipacket[2] & 0xfe)) << 1,
            .p = ((pb->ipacket[3] & 0x30) | (pb->ipacket[5] & 0x0e)) << 1,
            .w = (((pb->ipacket[5] & 0x01) << 2) | ((pb->ipacket[2] & 0x01) << 1) |
                  (pb->ipacket[1] & 0x01)) + 8,
            .flags = PSM_FINGER_FUZZY,
        };
      break;
    case 2:
      ew_finger_count = pb->ipacket[1] & 0x0f;
      // fallthrough
    default:
      break;
    }
    goto SYNAPTICS_END;

  case 1:
    if (sc->synhw.capReportsV && ew_finger_count > 3) {
      nfingers = ew_finger_count;
      break;
    }
    // fallthrough
  case 0:
    nfingers = w + 2;
    break;

  default:
    nfingers = 1;
  }

  if (sc->syninfo.touchpad_off)
    goto SYNAPTICS_END;

  touchpad_buttons = 0;
  if (pb->ipacket[0] & 0x01)
    touchpad_buttons |= FNU_MOUSE_BUTTON1DOWN;
  if (pb->ipacket[0] & 0x02)
    touchpad_buttons |= FNU_MOUSE_BUTTON3DOWN;

  if (sc->synhw.capExtended && sc->synhw.capFourButtons) {
    if ((pb->ipacket[3] ^ pb->ipacket[0]) & 0x01)
      touchpad_buttons |= FNU_MOUSE_BUTTON4DOWN;
    if ((pb->ipacket[3] ^ pb->ipacket[0]) & 0x02)
      touchpad_buttons |= FNU_MOUSE_BUTTON5DOWN;
  } else if (sc->synhw.capExtended && sc->synhw.capMiddle && !sc->synhw.capClickPad) {
    if ((pb->ipacket[0] ^ pb->ipacket[3]) & 0x01)
      touchpad_buttons |= FNU_MOUSE_BUTTON2DOWN;
  } else if (sc->synhw.capExtended && (sc->synhw.nExtendedButtons > 0)) {
    if ((pb->ipacket[0] ^ pb->ipacket[3]) & 0x02) {
      if (sc->syninfo.directional_scrolls) {
        if (pb->ipacket[4] & 0x01)
          extended_buttons |= FNU_MOUSE_BUTTON4DOWN;
        if (pb->ipacket[5] & 0x01)
          extended_buttons |= FNU_MOUSE_BUTTON5DOWN;
        if (pb->ipacket[4] & 0x02)
          extended_buttons |= FNU_MOUSE_BUTTON6DOWN;
        if (pb->ipacket[5] & 0x02)
          extended_buttons |= FNU_MOUSE_BUTTON7DOWN;
      } else {
        if (pb->ipacket[4] & 0x01)
          extended_buttons |= FNU_MOUSE_BUTTON1DOWN;
        if (pb->ipacket[5] & 0x01)
          extended_buttons |= FNU_MOUSE_BUTTON3DOWN;
        if (pb->ipacket[4] & 0x02)
          extended_buttons |= FNU_MOUSE_BUTTON2DOWN;
        sc->extended_buttons = extended_buttons;
      }

      int maskedbits = (sc->synhw.nExtendedButtons + 1) >> 1;
      int mask = (1 << maskedbits) - 1;
      pb->ipacket[4] &= ~(mask);
      pb->ipacket[5] &= ~(mask);
    } else if (!sc->syninfo.directional_scrolls && !sc->gesture.in_vscroll) {
      extended_buttons |= sc->extended_buttons;
    }
  }

  if (sc->synhw.capReportsV && nfingers > 1)
    f[0] = (finger_t){
        .x = ((pb->ipacket[3] & 0x10) << 8) | ((pb->ipacket[1] & 0x0f) << 8) |
             (pb->ipacket[4] & 0xfd),
        .y = ((pb->ipacket[3] & 0x20) << 7) | ((pb->ipacket[1] & 0xf0) << 4) |
             (pb->ipacket[5] & 0xfd),
        .p = *z & 0xfe,
        .w = (((pb->ipacket[2] & 0x01) << 2) | (pb->ipacket[5] & 0x02) |
              ((pb->ipacket[4] & 0x02) >> 1)) + 8,
        .flags = PSM_FINGER_FUZZY,
    };
  else
    f[0] = (finger_t){
        .x = ((pb->ipacket[3] & 0x10) << 8) | ((pb->ipacket[1] & 0x0f) << 8) |
             pb->ipacket[4],
        .y = ((pb->ipacket[3] & 0x20) << 7) | ((pb->ipacket[1] & 0xf0) << 4) |
             pb->ipacket[5],
        .p = *z,
        .w = w,
        .flags = nfingers > 1 ? PSM_FINGER_FUZZY : 0,
    };

  if (f[0].p < sc->syninfo.min_pressure || f[0].x < 2)
    nfingers = 0;

  if (sc->synhw.capClickPad) {
    clickpad_pressed = (pb->ipacket[0] ^ pb->ipacket[3]) & 0x01;
    if (sc->synhw.forcePad) {
      switch (nfingers) {
      case 0:
        sc->fpcount = 0;
        break;
      case 1:
        if (clickpad_pressed && sc->fpcount < 0x7fffffff)
          ++sc->fpcount;
        // fallthrough
      default:
        if (!clickpad_pressed)
          sc->fpcount = 0;
        if (sc->fpcount >= sc->syninfo.window_min)
          touchpad_buttons |= FNU_MOUSE_BUTTON1DOWN;
      }
    } else if (clickpad_pressed)
      touchpad_buttons |= FNU_MOUSE_BUTTON1DOWN;
  }

  for (id = 0; id < PSM_FINGERS; id++)
    if (id >= nfingers)
      PSM_FINGER_RESET(f[id]);

  psm_touch(sc, &f[0], nfingers, touchpad_buttons);
  ms->button = touchpad_buttons;

  palm = psmpalmdetect(sc, &f[0], nfingers);

  if (!palm)
    psmgestures(sc, &f[0], nfingers, ms);

  for (id = 0; id < PSM_FINGERS; id++)
    psmsmoother(sc, &f[id], id, ms, x, y);

  if (palm) {
    *x = *y = *z = 0;
    ms->button = ms->obutton;
    return 0;
  }

  ms->button |= extended_buttons | guest_buttons;

SYNAPTICS_END:
  if (ms->button & FNU_MOUSE_BUTTON4DOWN)
    *z = -1;
  else if (ms->button & FNU_MOUSE_BUTTON5DOWN)
    *z = 1;
  else if (ms->button & FNU_MOUSE_BUTTON6DOWN)
    *z = -2;
  else if (ms->button & FNU_MOUSE_BUTTON7DOWN)
    *z = 2;
  else
    *z = 0;
  ms->button &= ~(FNU_MOUSE_BUTTON4DOWN | FNU_MOUSE_BUTTON5DOWN | FNU_MOUSE_BUTTON6DOWN |
                  FNU_MOUSE_BUTTON7DOWN);

  return 0;
}

static int proc_synaptics_mux(struct psm_softc *sc, packetbuf_t *pb) {
  int butt;

  switch (pb->ipacket[0] & 0xc0) {
  case 0x80:
    memmove(sc->muxsave, pb->ipacket, 3);
    sc->muxmidtimeout = sc->lastsoftintr + 50000;
    return 1;

  case 0xc0:
    memmove(pb->ipacket + 3, pb->ipacket, 3);
    memmove(pb->ipacket, sc->muxsave, 3);
    pb->ipacket[0] &= ~(0x08 | sc->muxmsbuttons);
    pb->ipacket[3] &= ~(0x08 | sc->muxmsbuttons);
    butt = (pb->ipacket[3] & 0x03) << 2 | (pb->ipacket[0] & 0x03);
    if (butt != sc->muxtpbuttons && sc->fpcount < 1) {
      pb->ipacket[0] &= 0xfc;
      pb->ipacket[0] |= sc->muxtpbuttons & 0x03;
      pb->ipacket[3] &= 0xfc;
      pb->ipacket[3] |= sc->muxtpbuttons >> 2 & 0x03;
      ++sc->fpcount;
    } else {
      sc->fpcount = 0;
      sc->muxtpbuttons = butt;
    }
    if (sc->synhw.capExtended && !sc->synhw.capPassthrough &&
        (pb->ipacket[0] & 0x34) == 0x04 && (pb->ipacket[3] & 0x04) == 0x04) {
      pb->ipacket[0] &= 0xfb;
      pb->ipacket[3] &= 0xfb;
    }
    sc->muxsave[0] &= 0x30;
    break;

  default:
    if (sc->muxmsbuttons & 0x03 || pb->ipacket[0] & 0x03 ||
        (sc->lastsoftintr <= sc->muxmidtimeout &&
         (sc->muxsave[0] & 0x30 || sc->muxsave[2] > 8)))
      pb->ipacket[0] &= 0xfb;
    sc->muxmsbuttons = pb->ipacket[0] & 0x07;
    pb->ipacket[4] = pb->ipacket[1];
    pb->ipacket[5] = pb->ipacket[2];
    pb->ipacket[1] = pb->ipacket[0];
    pb->ipacket[2] = 0;
    pb->ipacket[0] = 0x84 | (sc->muxtpbuttons & 0x03);
    pb->ipacket[3] = 0xc4 | (sc->muxtpbuttons >> 2 & 0x03);
  }

  if (verbose >= 4)
    printf("synaptics: %x %x %x %x %x %x\n", pb->ipacket[0], pb->ipacket[1],
            pb->ipacket[2], pb->ipacket[3], pb->ipacket[4], pb->ipacket[5]);

  pb->inputbytes = MOUSE_SYNAPTICS_PACKETSIZE;
  return 0;
}

static int psmpalmdetect(struct psm_softc *sc, finger_t *f, int nfingers) {
  if (!(((sc->synhw.capMultiFinger || sc->synhw.capAdvancedGestures) &&
         !sc->synhw.capReportsV && nfingers > 1) ||
        (sc->synhw.capReportsV && nfingers > 2) ||
        (sc->synhw.capPalmDetect && f->w <= sc->syninfo.max_width) ||
        (!sc->synhw.capPalmDetect && f->p <= sc->syninfo.max_pressure) ||
        (sc->synhw.capPen && f->flags & PSM_FINGER_IS_PEN))) {
    if (verbose >= 2)
      printf("synaptics: palm detected! (%d)\n", f->w);
    return 1;
  }
  return 0;
}

static void psmgestures(struct psm_softc *sc, finger_t *fingers, int nfingers,
                        struct fnu_mousestatus *ms) {
  smoother_t *smoother;
  gesture_t *gest;
  finger_t *f;
  int y_ok, center_button, center_x, right_button, right_x, i;

  f = &fingers[0];
  smoother = &sc->smoother[0];
  gest = &sc->gesture;

  if (nfingers > 0) {
    for (i = 0; i < PSM_FINGERS; i++) {
      if (PSM_FINGER_IS_SET(fingers[i])) {
        f = &fingers[i];
        smoother = &sc->smoother[i];
        break;
      }
    }
  }

  if (f->p >= sc->syninfo.min_pressure) {
    int x0, y0;
    int dxp, dyp;
    int start_x, start_y;
    int queue_len;
    int margin_top, margin_right, margin_bottom, margin_left;
    int window_min, window_max;
    int vscroll_hor_area, vscroll_ver_area;
    int two_finger_scroll;
    int max_x, max_y;
    int three_finger_drag;

    margin_top = sc->syninfo.margin_top;
    margin_right = sc->syninfo.margin_right;
    margin_bottom = sc->syninfo.margin_bottom;
    margin_left = sc->syninfo.margin_left;
    window_min = sc->syninfo.window_min;
    window_max = sc->syninfo.window_max;
    vscroll_hor_area = sc->syninfo.vscroll_hor_area;
    vscroll_ver_area = sc->syninfo.vscroll_ver_area;
    two_finger_scroll = sc->syninfo.two_finger_scroll;
    max_x = sc->syninfo.max_x;
    max_y = sc->syninfo.max_y;
    three_finger_drag = sc->syninfo.three_finger_drag;
    x0 = f->x;
    y0 = f->y;

    if (x0 <= margin_left)
      x0 = margin_left;
    else if (x0 >= max_x - margin_right)
      x0 = max_x - margin_right;
    if (y0 <= margin_bottom)
      y0 = margin_bottom;
    else if (y0 >= max_y - margin_top)
      y0 = max_y - margin_top;

    if (verbose >= 3)
      printf("synaptics: ipacket: [%d, %d], %d, %d\n", x0, y0, f->p, f->w);

    if (!(sc->flags & PSM_FLAGS_FINGERDOWN)) {
      if (verbose >= 3)
        printf("synaptics: ----\n");

      gest->window_min = window_min;
      gest->zmax = 0;
      gest->fingers_nb = 0;
      gest->in_vscroll = 0;

      if (tap_enabled != 0)
        gest->taptimeout = sc->lastsoftintr + tap_timeout;
      else
        gest->taptimeout = 0;

      sc->flags |= PSM_FLAGS_FINGERDOWN;

      queue_len = 1;
      start_x = x0;
      start_y = y0;
    } else {
      queue_len = smoother->queue_len + 1;
      start_x = smoother->start_x;
      start_y = smoother->start_y;
    }

    if (sc->synhw.capClickPad && ms->button & FNU_MOUSE_BUTTON1DOWN) {
      y_ok = sc->syninfo.softbuttons_y >= 0 ? start_y < sc->syninfo.softbuttons_y
                                            : start_y > max_y + sc->syninfo.softbuttons_y;

      center_button = FNU_MOUSE_BUTTON2DOWN;
      center_x = sc->syninfo.softbutton2_x;
      right_button = FNU_MOUSE_BUTTON3DOWN;
      right_x = sc->syninfo.softbutton3_x;

      if (center_x > 0 && right_x > 0 && center_x > right_x) {
        center_button = FNU_MOUSE_BUTTON3DOWN;
        center_x = sc->syninfo.softbutton3_x;
        right_button = FNU_MOUSE_BUTTON2DOWN;
        right_x = sc->syninfo.softbutton2_x;
      }

      if (right_x > 0 && start_x > right_x && y_ok)
        ms->button = (ms->button & ~FNU_MOUSE_BUTTON1DOWN) | right_button;
      else if (center_x > 0 && start_x > center_x && y_ok)
        ms->button = (ms->button & ~FNU_MOUSE_BUTTON1DOWN) | center_button;
    }

    if (gest->in_taphold || (nfingers == 3 && three_finger_drag))
      ms->button |= gest->tap_button;

    if (nfingers > 1)
      gest->window_min = window_max;
    gest->fingers_nb = imax(nfingers, gest->fingers_nb);
    gest->zmax = imax(f->p, gest->zmax);

    if (queue_len < gest->window_min)
      return;

    dyp = -1;
    dxp = -1;

    // scrolling
    if (!gest->in_taphold && !ms->button && (!gest->in_vscroll || two_finger_scroll)) {
      dxp = iabs(x0 - start_x);
      dyp = iabs(y0 - start_y);

      if (sc->lastsoftintr > gest->taptimeout || dxp >= sc->syninfo.vscroll_min_delta ||
          dyp >= sc->syninfo.vscroll_min_delta) {
        if (two_finger_scroll) {
          if (nfingers == 2) {
            gest->in_vscroll += dyp ? 2 : 0;
            gest->in_vscroll += dxp ? 1 : 0;
          }
        } else {
          if ((vscroll_hor_area > 0 && start_y <= vscroll_hor_area) ||
              (vscroll_hor_area < 0 && start_y >= max_y + vscroll_hor_area))
            gest->in_vscroll += 2;

          if ((vscroll_ver_area > 0 && start_x <= vscroll_ver_area) ||
              (vscroll_ver_area < 0 && start_x >= max_x + vscroll_ver_area))
            gest->in_vscroll += 1;
        }

        if (gest->in_vscroll >= 3)
          gest->in_vscroll = (dxp > dyp) ? 2 : 1;
      }
    }
    if (two_finger_scroll && gest->in_vscroll != 0 && (nfingers != 2 || ms->button))
      gest->in_vscroll = 0;

    if (verbose >= 5)
      printf("synaptics: virtual scrolling: %s (direction=%d, dxp=%d, dyp=%d, fingers=%d)\n",
              gest->in_vscroll ? "YES" : "NO", gest->in_vscroll, dxp, dyp,
              gest->fingers_nb);

  } else if (sc->flags & PSM_FLAGS_FINGERDOWN) {
    // the finger left: a tap?
    int taphold_timeout, dx, dy, tap_max_delta;

    dx = iabs(smoother->queue[smoother->queue_cursor].x - smoother->start_x);
    dy = iabs(smoother->queue[smoother->queue_cursor].y - smoother->start_y);

    if (gest->fingers_nb > 1)
      tap_max_delta = imax(dx, dy);
    else
      tap_max_delta = sc->syninfo.tap_max_delta;

    sc->flags &= ~PSM_FLAGS_FINGERDOWN;

    if (verbose >= 3)
      printf("synaptics: zmax=%d, dx=%d, dy=%d, delta=%d, fingers=%d, queue=%d\n",
              gest->zmax, dx, dy, tap_max_delta, gest->fingers_nb, smoother->queue_len);
    if (!gest->in_vscroll && gest->zmax >= tap_threshold &&
        sc->lastsoftintr <= gest->taptimeout && dx <= tap_max_delta &&
        dy <= tap_max_delta && smoother->queue_len >= sc->syninfo.tap_min_queue) {
      if (gest->in_taphold) {
        gest->in_taphold = 0;
        if (verbose >= 2)
          printf("synaptics: button RELEASE: %d\n", gest->tap_button);
        sc->flags |= PSM_FLAGS_FINGERDOWN;
        sc->idletimeout = psmhz > 1 ? 1000000 / psmhz : 1000000;
      } else {
        gest->in_taphold = 1;
        taphold_timeout = sc->syninfo.taphold_timeout;
        gest->taptimeout = sc->lastsoftintr + taphold_timeout;
        sc->idletimeout = taphold_timeout;

        switch (gest->fingers_nb) {
        case 3:
          gest->tap_button = FNU_MOUSE_BUTTON2DOWN;
          break;
        case 2:
          gest->tap_button = FNU_MOUSE_BUTTON3DOWN;
          break;
        default:
          gest->tap_button = FNU_MOUSE_BUTTON1DOWN;
        }
        if (verbose >= 2)
          printf("synaptics: button PRESS: %d\n", gest->tap_button);
        ms->button |= gest->tap_button;
      }
    } else {
      if (gest->in_taphold) {
        if (verbose >= 2)
          printf("synaptics: button RELEASE: %d\n", gest->tap_button);
        gest->in_taphold = 0;
      } else if (verbose >= 2) {
        printf("synaptics: not a tap-hold\n");
      }
    }
  } else if (!(sc->flags & PSM_FLAGS_FINGERDOWN) && gest->in_taphold) {
    if (sc->lastsoftintr <= gest->taptimeout) {
      ms->button |= gest->tap_button;
    } else {
      if (verbose >= 2)
        printf("synaptics: button RELEASE: %d\n", gest->tap_button);
      gest->in_taphold = 0;
    }
  }
}

static void psmsmoother(struct psm_softc *sc, finger_t *f, int smoother_id,
                        struct fnu_mousestatus *ms, int *x, int *y) {
  smoother_t *smoother = &sc->smoother[smoother_id];
  gesture_t *gest = &(sc->gesture);

  if (f->p >= sc->syninfo.min_pressure) {
    int x0, y0;
    int cursor, peer, window;
    int dx, dy, dxp, dyp;
    int margin_top, margin_right, margin_bottom, margin_left;
    int na_top, na_right, na_bottom, na_left;
    int window_max;
    int multiplicator;
    int weight_current, weight_previous, weight_len_squared;
    int div_min, div_max, div_len;
    int two_finger_scroll;
    int max_x, max_y;
    int len, weight_prev_x, weight_prev_y;
    int div_max_x, div_max_y, div_x, div_y;
    int is_fuzzy;
    int natural_scroll;

    margin_top = sc->syninfo.margin_top;
    margin_right = sc->syninfo.margin_right;
    margin_bottom = sc->syninfo.margin_bottom;
    margin_left = sc->syninfo.margin_left;
    na_top = sc->syninfo.na_top;
    na_right = sc->syninfo.na_right;
    na_bottom = sc->syninfo.na_bottom;
    na_left = sc->syninfo.na_left;
    window_max = sc->syninfo.window_max;
    multiplicator = sc->syninfo.multiplicator;
    weight_current = sc->syninfo.weight_current;
    weight_previous = sc->syninfo.weight_previous;
    weight_len_squared = sc->syninfo.weight_len_squared;
    div_min = sc->syninfo.div_min;
    div_max = sc->syninfo.div_max;
    div_len = sc->syninfo.div_len;
    two_finger_scroll = sc->syninfo.two_finger_scroll;
    max_x = sc->syninfo.max_x;
    max_y = sc->syninfo.max_y;
    natural_scroll = sc->syninfo.natural_scroll;

    is_fuzzy = (f->flags & PSM_FINGER_FUZZY) != 0;

    x0 = f->x;
    y0 = f->y;

    if (x0 <= margin_left)
      x0 = margin_left;
    else if (x0 >= max_x - margin_right)
      x0 = max_x - margin_right;
    if (y0 <= margin_bottom)
      y0 = margin_bottom;
    else if (y0 >= max_y - margin_top)
      y0 = max_y - margin_top;

    if (smoother->active == 0) {
      if (verbose >= 3)
        printf("smoother%d: ---\n", smoother_id);

      smoother->start_x = x0;
      smoother->start_y = y0;
      dx = dy = 0;

      smoother->queue_cursor = SYNAPTICS_PACKETQUEUE;
      smoother->queue_len = 0;
      smoother->avg_dx = 0;
      smoother->avg_dy = 0;
      smoother->squelch_x = 0;
      smoother->squelch_y = 0;
      smoother->active = 1;
    } else {
      cursor = smoother->queue_cursor;
      dx = x0 - smoother->queue[cursor].x;
      dy = y0 - smoother->queue[cursor].y;
    }

    if (verbose >= 3)
      printf("smoother%d: ipacket: [%d, %d], %d, %d\n", smoother_id, x0, y0, f->p, f->w);

    cursor = SYNAPTICS_QUEUE_CURSOR(smoother->queue_cursor - 1);
    smoother->queue[cursor].x = x0;
    smoother->queue[cursor].y = y0;
    smoother->queue_cursor = cursor;
    if (smoother->queue_len < SYNAPTICS_PACKETQUEUE)
      smoother->queue_len++;
    if (verbose >= 5)
      printf("smoother%d: cursor[%d]: x=%d, y=%d, dx=%d, dy=%d\n", smoother_id,
              cursor, x0, y0, dx, dy);

    if (smoother->queue_len < gest->window_min)
      return;

    weight_prev_x = weight_prev_y = weight_previous;
    div_max_x = div_max_y = div_max;

    if (gest->in_vscroll) {
      div_min = sc->syninfo.vscroll_div_min;
      div_max_x = div_max_y = sc->syninfo.vscroll_div_max;
    } else {
      if (x0 <= na_left || x0 >= max_x - na_right) {
        weight_prev_x = sc->syninfo.weight_previous_na;
        div_max_x = sc->syninfo.div_max_na;
      }

      if (y0 <= na_bottom || y0 >= max_y - na_top) {
        weight_prev_y = sc->syninfo.weight_previous_na;
        div_max_y = sc->syninfo.div_max_na;
      }
    }

    window = imin(smoother->queue_len, window_max);
    peer = SYNAPTICS_QUEUE_CURSOR(cursor + window - 1);
    dxp = iabs(x0 - smoother->queue[peer].x) + 1;
    dyp = iabs(y0 - smoother->queue[peer].y) + 1;
    len = (dxp * dxp) + (dyp * dyp);
    weight_prev_x = imin(weight_prev_x, weight_len_squared * weight_prev_x / len);
    weight_prev_y = imin(weight_prev_y, weight_len_squared * weight_prev_y / len);

    len = (dxp + dyp) / 2;
    div_x = div_len * div_max_x / len;
    div_x = imin(div_max_x, div_x);
    div_x = imax(div_min, div_x);
    div_y = div_len * div_max_y / len;
    div_y = imin(div_max_y, div_y);
    div_y = imax(div_min, div_y);

    if (verbose >= 3)
      printf("smoother%d: peer=%d, len=%d, weight=%d/%d, div=%d/%d\n", smoother_id,
              peer, len, weight_prev_x, weight_prev_y, div_x, div_y);

    smoother->avg_dx = (weight_current * dx * multiplicator + weight_prev_x * smoother->avg_dx) /
                       (weight_current + weight_prev_x);

    smoother->avg_dy = (weight_current * dy * multiplicator + weight_prev_y * smoother->avg_dy) /
                       (weight_current + weight_prev_y);

    if (verbose >= 5)
      printf("smoother%d: avg_dx~=%d, avg_dy~=%d\n", smoother_id,
              smoother->avg_dx / multiplicator, smoother->avg_dy / multiplicator);

    smoother->squelch_x += smoother->avg_dx;
    dxp = smoother->squelch_x / (div_x * multiplicator);
    smoother->squelch_x = smoother->squelch_x % (div_x * multiplicator);

    smoother->squelch_y += smoother->avg_dy;
    dyp = smoother->squelch_y / (div_y * multiplicator);
    smoother->squelch_y = smoother->squelch_y % (div_y * multiplicator);

    switch (gest->in_vscroll) {
    case 0:
      if (is_fuzzy == smoother->is_fuzzy) {
        *x += dxp;
        *y += dyp;
      }
      if (verbose >= 3)
        printf("smoother%d: [%d, %d] -> [%d, %d]\n", smoother_id, dx, dy, dxp, dyp);
      break;
    case 1:
      if (dyp != 0) {
        if (two_finger_scroll && natural_scroll)
          ms->button |= (dyp > 0) ? FNU_MOUSE_BUTTON5DOWN : FNU_MOUSE_BUTTON4DOWN;
        else
          ms->button |= (dyp > 0) ? FNU_MOUSE_BUTTON4DOWN : FNU_MOUSE_BUTTON5DOWN;
      }
      break;
    case 2:
      if (dxp != 0) {
        if (two_finger_scroll && natural_scroll)
          ms->button |= (dxp > 0) ? FNU_MOUSE_BUTTON6DOWN : FNU_MOUSE_BUTTON7DOWN;
        else
          ms->button |= (dxp > 0) ? FNU_MOUSE_BUTTON7DOWN : FNU_MOUSE_BUTTON6DOWN;
      }
      break;
    }

    smoother->is_fuzzy = is_fuzzy;

  } else {
    smoother->active = 0;
  }
}

static int proc_elantech(struct psm_softc *sc, packetbuf_t *pb, struct fnu_mousestatus *ms,
                         int *x, int *y, int *z) {
  static int touchpad_button, trackpoint_button;
  finger_t fn, f[ELANTECH_MAX_FINGERS];
  int pkt, id, scale, i, nfingers, mask, palm;

  if (!elantech_support)
    return 0;

  if (ELANTECH_PKT_IS_DEBOUNCE(pb, sc->elanhw.hwversion))
    pkt = ELANTECH_PKT_NOP;
  else if (sc->elanhw.hastrackpoint && ELANTECH_PKT_IS_TRACKPOINT(pb))
    pkt = ELANTECH_PKT_TRACKPOINT;
  else
    switch (sc->elanhw.hwversion) {
    case 2:
      if (!ELANTECH_PKT_IS_V2(pb))
        return -1;
      pkt = (pb->ipacket[0] & 0xc0) == 0x80 ? ELANTECH_PKT_V2_2FINGER
                                            : ELANTECH_PKT_V2_COMMON;
      break;
    case 3:
      if (!ELANTECH_PKT_IS_V3_HEAD(pb, sc->elanhw.hascrc) &&
          !ELANTECH_PKT_IS_V3_TAIL(pb, sc->elanhw.hascrc))
        return -1;
      pkt = ELANTECH_PKT_V3;
      break;
    case 4:
      if (!ELANTECH_PKT_IS_V4(pb, sc->elanhw.hascrc))
        return -1;
      switch (pb->ipacket[3] & 0x03) {
      case 0x00:
        pkt = ELANTECH_PKT_V4_STATUS;
        break;
      case 0x01:
        pkt = ELANTECH_PKT_V4_HEAD;
        break;
      case 0x02:
        pkt = ELANTECH_PKT_V4_MOTION;
        break;
      default:
        return -1;
      }
      break;
    default:
      return -1;
    }

  if (verbose >= 5)
    printf("elantech: ipacket format: %d\n", pkt);

  for (id = 0; id < ELANTECH_MAX_FINGERS; id++)
    PSM_FINGER_RESET(f[id]);

  *x = *y = *z = 0;
  ms->button = ms->obutton;

  if (sc->syninfo.touchpad_off && pkt != ELANTECH_PKT_TRACKPOINT)
    return 0;

  switch (pkt) {
  case ELANTECH_PKT_V2_COMMON:
    // 0: N1 N0 W3 W2 . . R L   1: P7..P4 X11..X8   2: X7..X0
    // 3: N4 VF W1 W0 . . . B2   4: P3 P1 P2 P0 Y11..Y8   5: Y7..Y0
    nfingers = (pb->ipacket[0] & 0xc0) >> 6;
    if (nfingers == 3 && (pb->ipacket[3] & 0x80))
      nfingers = 4;

    if (nfingers == 0) {
      mask = (1 << nfingers) - 1;
      break;
    }

    mask = (1 << 1) - 1;
    f[0] = ELANTECH_FINGER_SET_XYP(pb);
    if (sc->elanhw.haspressure) {
      f[0].w = ((pb->ipacket[0] & 0x30) >> 2) | ((pb->ipacket[3] & 0x30) >> 4);
    } else {
      f[0].p = PSM_FINGER_DEFAULT_P;
      f[0].w = PSM_FINGER_DEFAULT_W;
    }

    if (nfingers > 2)
      f[0].flags = PSM_FINGER_FUZZY;
    break;

  case ELANTECH_PKT_V2_2FINGER:
    // 0: N1 N0 AY8 AX8 . . R L   1: AX7..AX0   2: AY7..AY0
    // 3: . . BY8 BX8 . . . .   4: BX7..BX0   5: BY7..BY0
    nfingers = 2;
    mask = (1 << nfingers) - 1;

    for (id = 0; id < imin(2, ELANTECH_MAX_FINGERS); id++)
      f[id] = (finger_t){
          .x = (((pb->ipacket[id * 3] & 0x10) << 4) | pb->ipacket[id * 3 + 1]) << 2,
          .y = (((pb->ipacket[id * 3] & 0x20) << 3) | pb->ipacket[id * 3 + 2]) << 2,
          .p = PSM_FINGER_DEFAULT_P,
          .w = PSM_FINGER_DEFAULT_W,
          .flags = PSM_FINGER_FUZZY};
    break;

  case ELANTECH_PKT_V3:
    // 0: N1 N0 W3 W2 0 1 R L   1: P7..P4 X11..X8   2: X7..X0
    // 3: 0 0 W1 W0 0 0 1 0   4: P3 P1 P2 P0 Y11..Y8   5: Y7..Y0
    nfingers = (pb->ipacket[0] & 0xc0) >> 6;
    id = nfingers > 2 ? 0 : nfingers - 1;
    mask = (1 << (id + 1)) - 1;

    if (nfingers == 0)
      break;

    fn = ELANTECH_FINGER_SET_XYP(pb);
    fn.w = ((pb->ipacket[0] & 0x30) >> 2) | ((pb->ipacket[3] & 0x30) >> 4);

    if (nfingers > 1)
      fn.flags = PSM_FINGER_FUZZY;

    if (nfingers == 2) {
      if (ELANTECH_PKT_IS_V3_HEAD(pb, sc->elanhw.hascrc)) {
        sc->elanaction.fingers[0] = fn;
        return 0;
      } else
        f[0] = sc->elanaction.fingers[0];
    }
    f[id] = fn;
    break;

  case ELANTECH_PKT_V4_STATUS:
    // 1: . . . F4 F3 F2 F1 F0   4: PL . . . . . . .
    mask = pb->ipacket[1] & 0x1f;
    nfingers = bitcount(mask);

    if (sc->elanaction.mask_v4wait != 0 && verbose >= 3)
      printf("elantech: HW v4 status packet when not all previous head packets received\n");

    sc->elanaction.mask_v4wait = mask & ~sc->elanaction.mask;

    if (sc->elanaction.mask_v4wait) {
      sc->elanaction.mask = mask;
      return 0;
    }
    break;

  case ELANTECH_PKT_V4_HEAD:
    // 0: W3..W0 0 1 R L   1: P7..P4 X11..X8   2: X7..X0
    // 3: ID2 ID1 ID0 1 0 0 0 1   4: P3 P1 P2 P0 Y11..Y8   5: Y7..Y0
    mask = sc->elanaction.mask;
    nfingers = bitcount(mask);
    id = ((pb->ipacket[3] & 0xe0) >> 5) - 1;
    fn = ELANTECH_FINGER_SET_XYP(pb);
    fn.w = (pb->ipacket[0] & 0xf0) >> 4;

    if (id < 0)
      return 0;

    if (sc->elanaction.mask_v4wait == 0) {
      if (id < ELANTECH_MAX_FINGERS)
        f[id] = fn;
      break;
    }

    sc->elanaction.mask_v4wait &= ~(1 << id);
    if (id < ELANTECH_MAX_FINGERS)
      sc->elanaction.fingers[id] = fn;

    if (sc->elanaction.mask_v4wait != 0)
      return 0;

    for (id = 0; id < ELANTECH_MAX_FINGERS; id++)
      if (sc->elanaction.mask & (1 << id))
        f[id] = sc->elanaction.fingers[id];
    break;

  case ELANTECH_PKT_V4_MOTION:
    // 0: ID2 ID1 ID0 OF 0 1 R L   1: DX   2: DY   3: ID 1 0 0 1 0   4: DX  5: DY
    mask = sc->elanaction.mask;
    nfingers = bitcount(mask);

    for (id = 0; id < ELANTECH_MAX_FINGERS; id++)
      if (sc->elanaction.mask & (1 << id))
        f[id] = sc->elanaction.fingers[id];

    scale = (pb->ipacket[0] & 0x10) ? 5 : 1;
    for (i = 0; i <= 3; i += 3) {
      id = ((pb->ipacket[i] & 0xe0) >> 5) - 1;
      if (id < 0 || id >= ELANTECH_MAX_FINGERS)
        continue;

      if (PSM_FINGER_IS_SET(sc->elanaction.fingers[id])) {
        f[id] = sc->elanaction.fingers[id];
        f[id].x += imax(-f[id].x, (int8_t)pb->ipacket[i + 1] * scale);
        f[id].y += imax(-f[id].y, (int8_t)pb->ipacket[i + 2] * scale);
      } else if (verbose >= 3) {
        printf("elantech: HW v4 motion packet skipped\n");
      }
    }
    break;

  case ELANTECH_PKT_TRACKPOINT:
    // 0: 0 0 SY SX 0 M R L   1: ~SX 0..   2: ~SY 0..   3: 0 0 ~SY ~SX 0 1 1 0
    // 4: X7..X0   5: Y7..Y0
    if (!(pb->ipacket[0] & 0xC8) && !(pb->ipacket[1] & 0x7F) &&
        !(pb->ipacket[2] & 0x7F) && !(pb->ipacket[3] & 0xC9) &&
        !(pb->ipacket[0] & 0x10) != !(pb->ipacket[1] & 0x80) &&
        !(pb->ipacket[0] & 0x10) != !(pb->ipacket[3] & 0x10) &&
        !(pb->ipacket[0] & 0x20) != !(pb->ipacket[2] & 0x80) &&
        !(pb->ipacket[0] & 0x20) != !(pb->ipacket[3] & 0x20)) {
      *x = (pb->ipacket[0] & FNU_MOUSE_PS2_XNEG) ? pb->ipacket[4] - 256 : pb->ipacket[4];
      *y = (pb->ipacket[0] & FNU_MOUSE_PS2_YNEG) ? pb->ipacket[5] - 256 : pb->ipacket[5];

      trackpoint_button = ((pb->ipacket[0] & 0x01) ? FNU_MOUSE_BUTTON1DOWN : 0) |
                          ((pb->ipacket[0] & 0x02) ? FNU_MOUSE_BUTTON3DOWN : 0) |
                          ((pb->ipacket[0] & 0x04) ? FNU_MOUSE_BUTTON2DOWN : 0);
      ms->button = touchpad_button | trackpoint_button;
    } else if (verbose >= 3)
      printf("elantech: unexpected trackpoint packet skipped\n");
    return 0;

  case ELANTECH_PKT_NOP:
    return 0;

  default:
    return -1;
  }

  for (id = 0; id < ELANTECH_MAX_FINGERS; id++)
    if (PSM_FINGER_IS_SET(f[id]) && verbose >= 2)
      printf("elantech: finger %d: down [%d, %d], %d, %d, %d\n", id + 1, f[id].x,
              f[id].y, f[id].p, f[id].w, f[id].flags);

  if (sc->elanhw.isclickpad) {
    touchpad_button = ((pb->ipacket[0] & 0x03) ? FNU_MOUSE_BUTTON1DOWN : 0);
  } else {
    touchpad_button = ((pb->ipacket[0] & 0x01) ? FNU_MOUSE_BUTTON1DOWN : 0) |
                      ((pb->ipacket[0] & 0x02) ? FNU_MOUSE_BUTTON3DOWN : 0);
    if (sc->elanhw.has3buttons)
      touchpad_button |= ((pb->ipacket[0] & 0x04) ? FNU_MOUSE_BUTTON2DOWN : 0);
  }

  psm_touch(sc, &f[0], nfingers, touchpad_button);
  ms->button = touchpad_button | trackpoint_button;

  palm = psmpalmdetect(sc, &f[0], nfingers);

  if ((PSM_FINGER_IS_SET(f[0]) || PSM_FINGER_IS_SET(f[1]) || nfingers == 0) && !palm)
    psmgestures(sc, &f[0], imin(nfingers, 3), ms);

  for (id = 0; id < PSM_FINGERS; id++)
    if (PSM_FINGER_IS_SET(f[id]) || !(mask & (1 << id)))
      psmsmoother(sc, &f[id], id, ms, x, y);

  for (id = 0; id < ELANTECH_MAX_FINGERS; id++) {
    if (PSM_FINGER_IS_SET(f[id]))
      sc->elanaction.fingers[id] = f[id];
    if ((sc->elanaction.mask & (1 << id)) && !(mask & (1 << id)))
      PSM_FINGER_RESET(sc->elanaction.fingers[id]);
  }
  sc->elanaction.mask = mask;

  if (palm) {
    *x = *y = *z = 0;
    ms->button = ms->obutton;
    return 0;
  }

  if (ms->button & FNU_MOUSE_BUTTON4DOWN)
    *z = -1;
  else if (ms->button & FNU_MOUSE_BUTTON5DOWN)
    *z = 1;
  else if (ms->button & FNU_MOUSE_BUTTON6DOWN)
    *z = -2;
  else if (ms->button & FNU_MOUSE_BUTTON7DOWN)
    *z = 2;
  else
    *z = 0;
  ms->button &= ~(FNU_MOUSE_BUTTON4DOWN | FNU_MOUSE_BUTTON5DOWN | FNU_MOUSE_BUTTON6DOWN |
                  FNU_MOUSE_BUTTON7DOWN);

  return 0;
}

// 3: 1 1 1 A 1 L T R, 4: V11..V8 H11..H8, 5: 0 P6..P0; a tap is button 4
static void proc_versapad(struct psm_softc *sc, packetbuf_t *pb, struct fnu_mousestatus *ms,
                          int *x, int *y, int *z __unused) {
  static int butmap_versapad[8] = {
      0,
      FNU_MOUSE_BUTTON3DOWN,
      0,
      FNU_MOUSE_BUTTON3DOWN,
      FNU_MOUSE_BUTTON1DOWN,
      FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN,
      FNU_MOUSE_BUTTON1DOWN,
      FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN};
  int c, x0, y0;

  c = pb->ipacket[0];
  *x = *y = 0;
  ms->button = butmap_versapad[c & MOUSE_PS2VERSA_BUTTONS];
  ms->button |= (c & MOUSE_PS2VERSA_TAP) ? FNU_MOUSE_BUTTON4DOWN : 0;
  if (c & MOUSE_PS2VERSA_IN_USE) {
    x0 = pb->ipacket[1] | (((pb->ipacket[4]) & 0x0f) << 8);
    y0 = pb->ipacket[2] | (((pb->ipacket[4]) & 0xf0) << 4);
    if (x0 & 0x800)
      x0 -= 0x1000;
    if (y0 & 0x800)
      y0 -= 0x1000;
    if (sc->flags & PSM_FLAGS_FINGERDOWN) {
      *x = sc->xold - x0;
      *y = y0 - sc->yold;
      if (*x < 0)
        ++*x;
      else if (*x)
        --*x;
      if (*y < 0)
        ++*y;
      else if (*y)
        --*y;
    } else
      sc->flags |= PSM_FLAGS_FINGERDOWN;
    sc->xold = x0;
    sc->yold = y0;
  } else
    sc->flags &= ~PSM_FLAGS_FINGERDOWN;
}

// THE INTERRUPT

static void psmsoftintridle(struct psm_softc *sc) {
  packetbuf_t *pb;

  if (sc->pqueue_start == sc->pqueue_end && sc->idlepacket.inputbytes > 0) {
    if (--sc->pqueue_start < 0)
      sc->pqueue_start = PSM_PACKETQUEUE - 1;

    pb = &sc->pqueue[sc->pqueue_start];
    memmove(pb, &sc->idlepacket, sizeof(packetbuf_t));
    if (verbose >= 4)
      printf("psmsoftintridle: %x %x %x %x %x %x\n", pb->ipacket[0], pb->ipacket[1],
              pb->ipacket[2], pb->ipacket[3], pb->ipacket[4], pb->ipacket[5]);
    psmsoftintr(sc);
  }
}

// /dev/psm: the bytes its reader takes, kbd_lock held
static int psm_queue(struct psm_softc *sc, const uchar *p, int n) {
  int l;

  if (n <= 0 || sc->queue.count + n >= PSM_BUFSIZE)
    return 0;
  l = n < PSM_BUFSIZE - sc->queue.tail ? n : PSM_BUFSIZE - sc->queue.tail;
  memmove(&sc->queue.buf[sc->queue.tail], p, (size_t)l);
  if (n > l)
    memmove(&sc->queue.buf[0], p + l, (size_t)(n - l));
  sc->queue.tail = (sc->queue.tail + n) % PSM_BUFSIZE;
  sc->queue.count += n;
  return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// levels 0 and 1: a PS/2 or a sysmouse packet
static int tame_mouse(struct psm_softc *sc, struct fnu_mousestatus *status, uchar *buf) {
  static const uchar butmapps2[8] = {
      0,
      FNU_MOUSE_PS2_BUTTON1DOWN,
      FNU_MOUSE_PS2_BUTTON2DOWN,
      FNU_MOUSE_PS2_BUTTON1DOWN | FNU_MOUSE_PS2_BUTTON2DOWN,
      FNU_MOUSE_PS2_BUTTON3DOWN,
      FNU_MOUSE_PS2_BUTTON1DOWN | FNU_MOUSE_PS2_BUTTON3DOWN,
      FNU_MOUSE_PS2_BUTTON2DOWN | FNU_MOUSE_PS2_BUTTON3DOWN,
      FNU_MOUSE_PS2_BUTTON1DOWN | FNU_MOUSE_PS2_BUTTON2DOWN | FNU_MOUSE_PS2_BUTTON3DOWN,
  };
  static const uchar butmapmsc[8] = {
      FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON2UP | FNU_MOUSE_MSC_BUTTON3UP,
      FNU_MOUSE_MSC_BUTTON2UP | FNU_MOUSE_MSC_BUTTON3UP,
      FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON3UP,
      FNU_MOUSE_MSC_BUTTON3UP,
      FNU_MOUSE_MSC_BUTTON1UP | FNU_MOUSE_MSC_BUTTON2UP,
      FNU_MOUSE_MSC_BUTTON2UP,
      FNU_MOUSE_MSC_BUTTON1UP,
      0,
  };
  int mapped, i;

  if (sc->mode.level == PSM_LEVEL_BASE) {
    mapped = status->button & ~FNU_MOUSE_BUTTON4DOWN;
    if (status->button & FNU_MOUSE_BUTTON4DOWN)
      mapped |= FNU_MOUSE_BUTTON1DOWN;
    buf[0] = (uchar)(FNU_MOUSE_PS2_SYNC | butmapps2[mapped & FNU_MOUSE_STDBUTTONS]);
    i = clampi(status->dx, -256, 255);
    if (i < 0)
      buf[0] |= FNU_MOUSE_PS2_XNEG;
    buf[1] = (uchar)i;
    i = clampi(status->dy, -256, 255);
    if (i < 0)
      buf[0] |= FNU_MOUSE_PS2_YNEG;
    buf[2] = (uchar)i;
    return FNU_MOUSE_PS2_PACKETSIZE;
  }
  buf[0] = (uchar)(FNU_MOUSE_MSC_SYNC | butmapmsc[status->button & FNU_MOUSE_STDBUTTONS]);
  i = clampi(status->dx, -256, 255);
  buf[1] = (uchar)(i >> 1);
  buf[3] = (uchar)(i - (int8_t)buf[1]);
  i = clampi(status->dy, -256, 255);
  buf[2] = (uchar)(i >> 1);
  buf[4] = (uchar)(i - (int8_t)buf[2]);
  i = clampi(status->dz, -128, 127);
  buf[5] = (uchar)((i >> 1) & 0x7f);
  buf[6] = (uchar)((i - (i >> 1)) & 0x7f);
  buf[7] = (uchar)((~status->button >> 3) & 0x7f);
  return FNU_MOUSE_SYS_PACKETSIZE;
}

// PS/2
static uint32 ws_buttons(int button) {
  uint32 b = 0;

  if (button & FNU_MOUSE_BUTTON1DOWN)
    b |= 1;
  if (button & FNU_MOUSE_BUTTON3DOWN)
    b |= 2;
  if (button & FNU_MOUSE_BUTTON2DOWN)
    b |= 4;
  return b | ((uint32)button & ~7u);
}

static void psm_output(struct psm_softc *sc, struct fnu_mousestatus *ms) {
  wsmouse_input(sc->unit, ms->dx, ms->dy, -ms->dz, ws_buttons(ms->button));
}

static void psmsoftintr(struct psm_softc *sc) {
  static int butmap[8] = {
      0,
      FNU_MOUSE_BUTTON1DOWN,
      FNU_MOUSE_BUTTON3DOWN,
      FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN,
      FNU_MOUSE_BUTTON2DOWN,
      FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON2DOWN,
      FNU_MOUSE_BUTTON2DOWN | FNU_MOUSE_BUTTON3DOWN,
      FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON2DOWN | FNU_MOUSE_BUTTON3DOWN};
  struct fnu_mousestatus ms;
  uchar tamed[FNU_MOUSE_SYS_PACKETSIZE];
  packetbuf_t *pb;
  int x, y, z, c, queued = 0;

  sc->lastsoftintr = now_us();

  do {
    pb = &sc->pqueue[sc->pqueue_start];

    if ((sc->state & PSM_DEVOPEN) && sc->mode.level == PSM_LEVEL_NATIVE)
      queued |= psm_queue(sc, pb->ipacket, pb->inputbytes);

    c = pb->ipacket[0];
    if (sc->hw.model == FNU_MOUSE_MODEL_THINK)
      pb->ipacket[1] |= (c & FNU_MOUSE_PS2_XOVERFLOW) ? 0x80 : 0;

    x = (c & FNU_MOUSE_PS2_XNEG) ? pb->ipacket[1] - 256 : pb->ipacket[1];
    y = (c & FNU_MOUSE_PS2_YNEG) ? pb->ipacket[2] - 256 : pb->ipacket[2];
    z = 0;
    ms.obutton = sc->button;
    ms.button = butmap[c & FNU_MOUSE_PS2_BUTTONS];
    if (sc->config & PSM_CONFIG_FORCETAP)
      ms.button |= ((c & FNU_MOUSE_PS2_TAP)) ? 0 : FNU_MOUSE_BUTTON4DOWN;
    sc->idletimeout = 0;
    sc->idlepacket.inputbytes = 0;

    switch (sc->hw.model) {
    case FNU_MOUSE_MODEL_EXPLORER:
      // byte 4: * * S2 S1 s d2 d1 d0
      z = (pb->ipacket[3] & MOUSE_EXPLORER_ZNEG) ? (pb->ipacket[3] & 0x0f) - 16
                                                 : (pb->ipacket[3] & 0x0f);
      ms.button |= (pb->ipacket[3] & MOUSE_EXPLORER_BUTTON4DOWN) ? FNU_MOUSE_BUTTON4DOWN : 0;
      ms.button |= (pb->ipacket[3] & MOUSE_EXPLORER_BUTTON5DOWN) ? FNU_MOUSE_BUTTON5DOWN : 0;
      break;

    case FNU_MOUSE_MODEL_INTELLI:
    case FNU_MOUSE_MODEL_NET:
      z = (int8_t)pb->ipacket[3];
      if ((z >= 7) || (z <= -7))
        z = 0;
      ms.button |= (c & MOUSE_PS2INTELLI_BUTTON4DOWN) ? FNU_MOUSE_BUTTON4DOWN : 0;
      ms.button |= (c & MOUSE_PS2INTELLI_BUTTON5DOWN) ? FNU_MOUSE_BUTTON5DOWN : 0;
      break;

    case FNU_MOUSE_MODEL_MOUSEMANPLUS:
      proc_mmanplus(sc, pb, &ms, &x, &y, &z);
      break;

    case FNU_MOUSE_MODEL_GLIDEPOINT:
      ms.button |= ((c & FNU_MOUSE_PS2_TAP)) ? 0 : FNU_MOUSE_BUTTON4DOWN;
      break;

    case FNU_MOUSE_MODEL_NETSCROLL:
      ms.button |= (pb->ipacket[3] & FNU_MOUSE_PS2_BUTTONS & 0x02) ? FNU_MOUSE_BUTTON4DOWN : 0;
      ms.button |= (pb->ipacket[3] & FNU_MOUSE_PS2_BUTTONS & 0x01) ? FNU_MOUSE_BUTTON5DOWN : 0;
      z = (pb->ipacket[3] & FNU_MOUSE_PS2_XNEG) ? pb->ipacket[4] - 256 : pb->ipacket[4];
      break;

    case FNU_MOUSE_MODEL_THINK:
      ms.button |= (c & FNU_MOUSE_PS2_TAP) ? FNU_MOUSE_BUTTON4DOWN : 0;
      break;

    case FNU_MOUSE_MODEL_VERSAPAD:
      proc_versapad(sc, pb, &ms, &x, &y, &z);
      c = ((x < 0) ? FNU_MOUSE_PS2_XNEG : 0) | ((y < 0) ? FNU_MOUSE_PS2_YNEG : 0);
      break;

    case FNU_MOUSE_MODEL_4D:
      // byte 1: s2 d2 s1 d1 1 M R L, bytes 2 and 3 signed x and y
      x = (pb->ipacket[1] & 0x80) ? pb->ipacket[1] - 256 : pb->ipacket[1];
      y = (pb->ipacket[2] & 0x80) ? pb->ipacket[2] - 256 : pb->ipacket[2];
      switch (c & MOUSE_4D_WHEELBITS) {
      case 0x10:
        z = 1;
        break;
      case 0x30:
        z = -1;
        break;
      case 0x40:
        z = 2;
        break;
      case 0xc0:
        z = -2;
        break;
      }
      break;

    case FNU_MOUSE_MODEL_4DPLUS:
      // byte 1: 0 0 1 1 1 M R L, byte 2: 0 0 0 0 1 0 0 0, byte 3: 0 0 0 0 S s d1 d0
      if ((x < 16 - 256) && (y < 16 - 256)) {
        x = y = 0;
        if (pb->ipacket[2] & MOUSE_4DPLUS_BUTTON4DOWN)
          ms.button |= FNU_MOUSE_BUTTON4DOWN;
        z = (pb->ipacket[2] & MOUSE_4DPLUS_ZNEG) ? ((pb->ipacket[2] & 0x07) - 8)
                                                 : (pb->ipacket[2] & 0x07);
      } else {
        ms.button |= ms.obutton & FNU_MOUSE_EXTBUTTONS;
      }
      break;

    case FNU_MOUSE_MODEL_SYNAPTICS:
      if (pb->inputbytes == FNU_MOUSE_PS2_PACKETSIZE)
        if (proc_synaptics_mux(sc, pb))
          goto next;

      if (proc_synaptics(sc, pb, &ms, &x, &y, &z) != 0) {
        if (verbose >= 3)
          printf("synaptics: packet rejected\n");
        goto next;
      }
      break;

    case FNU_MOUSE_MODEL_ELANTECH:
      if (proc_elantech(sc, pb, &ms, &x, &y, &z) != 0) {
        if (verbose >= 3)
          printf("elantech: packet rejected\n");
        goto next;
      }
      break;

    case FNU_MOUSE_MODEL_TRACKPOINT:
    case FNU_MOUSE_MODEL_GENERIC:
    default:
      break;
    }

    if (sc->idletimeout != 0 && sc->idlepacket.inputbytes == 0)
      sc->idlepacket = *pb;

    if (sc->mode.accelfactor >= 1) {
      if (x != 0) {
        x = x * x / sc->mode.accelfactor;
        if (x == 0)
          x = 1;
        if (c & FNU_MOUSE_PS2_XNEG)
          x = -x;
      }
      if (y != 0) {
        y = y * y / sc->mode.accelfactor;
        if (y == 0)
          y = 1;
        if (c & FNU_MOUSE_PS2_YNEG)
          y = -y;
      }
    }

    ms.dx = x;
    ms.dy = y;
    ms.dz = z;
    ms.flags = ((x || y || z) ? FNU_MOUSE_POSCHANGED : 0) | (ms.obutton ^ ms.button);

    sc->status.flags |= ms.flags;
    sc->status.dx += ms.dx;
    sc->status.dy += ms.dy;
    sc->status.dz += ms.dz;
    sc->status.button = ms.button;
    sc->button = ms.button;

    if (ms.flags != 0)
      psm_output(sc, &ms);
    if ((sc->state & PSM_DEVOPEN) && sc->mode.level != PSM_LEVEL_NATIVE)
      queued |= psm_queue(sc, tamed, tame_mouse(sc, &ms, tamed));

    sc->watchdog = 0;

  next:
    pb->inputbytes = 0;
    if (++sc->pqueue_start >= PSM_PACKETQUEUE)
      sc->pqueue_start = 0;
  } while (sc->pqueue_start != sc->pqueue_end);

  if (queued) {
    if (sc->state & PSM_ASLP) {
      sc->state &= ~PSM_ASLP;
      wakeup(&sc->queue);
    }
    selwakeup(&sc->rsel);
  }
  sc->state &= ~PSM_SOFTARMED;
  callout_stop_async(&sc->softcallout);

  if (sc->idletimeout != 0) {
    sc->state |= PSM_SOFTARMED;
    callout_reset(&sc->softcallout,
                  (int)((sc->idletimeout * HZ + 999999) / 1000000),
                  psmsoftintridle_callout, sc);
    if (verbose >= 2)
      printf("softintr: callout set: %d us\n", (int)sc->idletimeout);
  }
}

static void psmintr(struct psm_softc *sc) {
  int64_t now;
  int c;
  packetbuf_t *pb;

  if (aux_mux_is_enabled(sc->kbdc) && verbose >= 2)
    printf("psmintr: active multiplexing mode is not supported!\n");

  while ((c = read_aux_data_no_wait(sc->kbdc)) != -1) {
    pb = &sc->pqueue[sc->pqueue_end];

    if (!(sc->state & PSM_OPEN))
      continue;

    now = now_us();
    if ((pb->inputbytes > 0) && now > sc->inputtimeout) {
      if (verbose >= 3)
        printf("psmintr: delay too long; resetting byte count\n");
      pb->inputbytes = 0;
      sc->syncerrors = 0;
      sc->pkterrors = 0;
    }
    sc->inputtimeout = now + PSM_INPUT_TIMEOUT;

    pb->ipacket[pb->inputbytes++] = (uchar)c;

    if (pb->inputbytes < sc->mode.packetsize)
      continue;
    if (verbose >= 4)
      printf("psmintr: %x %x %x %x %x %x\n", pb->ipacket[0], pb->ipacket[1],
              pb->ipacket[2], pb->ipacket[3], pb->ipacket[4], pb->ipacket[5]);

    c = pb->ipacket[0];

    if ((sc->flags & PSM_NEED_SYNCBITS) != 0) {
      sc->mode.syncmask[1] = (uchar)(c & sc->mode.syncmask[0]);
      sc->flags &= ~PSM_NEED_SYNCBITS;
      if (verbose >= 2)
        printf("psmintr: Sync bytes now %x,%x\n", sc->mode.syncmask[0],
                sc->mode.syncmask[1]);
    } else if ((sc->config & PSM_CONFIG_NOCHECKSYNC) == 0 &&
               (c & sc->mode.syncmask[0]) != sc->mode.syncmask[1]) {
      if (verbose >= 3)
        printf("psmintr: out of sync (%x != %x) %d cmds since last error.\n",
                c & sc->mode.syncmask[0], sc->mode.syncmask[1],
                sc->cmdcount - sc->lasterr);
      sc->lasterr = sc->cmdcount;
      dropqueue(sc);
      if (sc->syncerrors == 0)
        sc->pkterrors++;
      ++sc->syncerrors;
      sc->lastinputerr = now;
      if (sc->syncerrors >= sc->mode.packetsize * 2 || sc->pkterrors >= pkterrthresh) {
        if (verbose >= 3)
          printf("psmintr: reset the mouse.\n");
        sc->recover_reset = 1;
        task_add(systq, &sc->recover);
      } else if (sc->syncerrors == sc->mode.packetsize) {
        if (verbose >= 3)
          printf("psmintr: re-enable the mouse.\n");
        pb->inputbytes = 0;
        task_add(systq, &sc->recover);
      } else {
        if (verbose >= 3)
          printf("psmintr: discard a byte (%d)\n", sc->syncerrors);
        pb->inputbytes--;
        memmove(&pb->ipacket[0], &pb->ipacket[1], pb->inputbytes);
      }
      continue;
    }

    sc->syncerrors = 0;

    if (!timeelapsed(sc->lastinputerr, psmerrsecs, psmerrusecs, now)) {
      pb->inputbytes = 0;
      continue;
    }

    sc->pkterrors = 0;

    sc->cmdcount++;
    if (++sc->pqueue_end >= PSM_PACKETQUEUE)
      sc->pqueue_end = 0;
    if (sc->hw.model == FNU_MOUSE_MODEL_SYNAPTICS || sc->hw.model == FNU_MOUSE_MODEL_ELANTECH ||
        !timeelapsed(sc->lastsoftintr, psmsecs, psmusecs, now) ||
        (sc->pqueue_end == sc->pqueue_start)) {
      if ((sc->state & PSM_SOFTARMED) != 0) {
        sc->state &= ~PSM_SOFTARMED;
        callout_stop_async(&sc->softcallout);
      }
      psmsoftintr(sc);
    } else if ((sc->state & PSM_SOFTARMED) == 0) {
      sc->state |= PSM_SOFTARMED;
      callout_reset(&sc->softcallout, psmhz < 1 ? 1 : HZ / psmhz,
                    psmsoftintr_callout, sc);
    }
  }
}

// THE CALLOUTS
static void psmsoftintr_callout(void *arg) {
  struct psm_softc *sc = arg;

  mtx_enter(&kbd_lock);
  if ((sc->state & PSM_OPEN) && sc->pqueue_start != sc->pqueue_end)
    psmsoftintr(sc);
  mtx_leave(&kbd_lock);
}

static void psmsoftintridle_callout(void *arg) {
  struct psm_softc *sc = arg;

  mtx_enter(&kbd_lock);
  if (sc->state & PSM_OPEN)
    psmsoftintridle(sc);
  mtx_leave(&kbd_lock);
}

static void psmtimeout(void *arg) {
  struct psm_softc *sc = arg;

  mtx_enter(&kbd_lock);
  if (!(sc->state & PSM_OPEN)) {
    mtx_leave(&kbd_lock);
    return;
  }
  if (sc->watchdog && kbdc_lock(sc->kbdc, 1)) {
    if (verbose >= 6)
      printf("psm: lost interrupt?\n");
    psmintr(sc);
    kbdc_lock(sc->kbdc, 0);
  }
  sc->watchdog = 1;
  callout_reset(&sc->callout, HZ, psmtimeout, sc);
  mtx_leave(&kbd_lock);
}

// kbd_lock held
void psm_intr_locked(void) {
  struct psm_softc *sc = &psm_sc;

  if (sc->state & PSM_OPEN)
    psmintr(sc);
}

// reset, or off and on, out of the interrupt
static void psm_recover(void *arg) {
  struct psm_softc *sc = arg;
  int reset;

  mtx_enter(&kbd_lock);
  reset = sc->recover_reset;
  sc->recover_reset = 0;
  mtx_leave(&kbd_lock);
  if (reset) {
    reinitialize(sc, 1);
    return;
  }
  if (!kbdc_lock(sc->kbdc, 1))
    return;
  disable_aux_dev(sc->kbdc);
  enable_aux_dev(sc->kbdc);
  kbdc_lock(sc->kbdc, 0);
}

// VENDOR SPECIFIC

static int mouse_id_proc1(KBDC kbdc, int res, int scale, int *status) {
  if (set_mouse_resolution(kbdc, res) != res)
    return 0;
  if (set_mouse_scaling(kbdc, scale) && set_mouse_scaling(kbdc, scale) &&
      set_mouse_scaling(kbdc, scale) && (get_mouse_status(kbdc, status, 0, 3) >= 3))
    return 1;
  return 0;
}

static int mouse_ext_command(KBDC kbdc, int command) {
  int c;

  c = (command >> 6) & 0x03;
  if (set_mouse_resolution(kbdc, c) != c)
    return 0;
  c = (command >> 4) & 0x03;
  if (set_mouse_resolution(kbdc, c) != c)
    return 0;
  c = (command >> 2) & 0x03;
  if (set_mouse_resolution(kbdc, c) != c)
    return 0;
  c = (command >> 0) & 0x03;
  if (set_mouse_resolution(kbdc, c) != c)
    return 0;
  return 1;
}

static int enable_groller(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  int status[3];

  if (!mouse_id_proc1(kbdc, PSMD_RES_HIGH, 1, status))
    return 0;
  if ((status[1] != '3') || (status[2] != 'D'))
    return 0;
  if (arg == PROBE)
    sc->hw.buttons = 4;
  return 1;
}

static int enable_gmouse(struct psm_softc *sc, enum probearg arg __unused) {
  KBDC kbdc = sc->kbdc;
  int status[3];

  if (!mouse_id_proc1(kbdc, PSMD_RES_HIGH, 1, status))
    return 0;
  if ((status[1] != '3') || (status[2] != 'U'))
    return 0;
  return 1;
}

static int enable_aglide(struct psm_softc *sc, enum probearg arg __unused) {
  KBDC kbdc = sc->kbdc;
  int status[3];

  if (set_mouse_sampling_rate(kbdc, 100) != 100)
    return 0;
  if (!mouse_id_proc1(kbdc, PSMD_RES_LOW, 2, status))
    return 0;
  if ((status[1] == PSMD_RES_LOW) || (status[2] == 100))
    return 0;
  return 1;
}

// Kensington ThinkingMouse/Trackball
static int enable_kmouse(struct psm_softc *sc, enum probearg arg __unused) {
  static uchar rate[] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
  KBDC kbdc = sc->kbdc;
  int status[3], id1, id2, i;

  id1 = get_aux_id(kbdc);
  if (set_mouse_sampling_rate(kbdc, 10) != 10)
    return 0;
  id2 = get_aux_id(kbdc);
  if ((id1 == id2) || (id2 != 2))
    return 0;

  if (set_mouse_resolution(kbdc, PSMD_RES_LOW) != PSMD_RES_LOW)
    return 0;

  for (i = 0; i < (int)sizeof(rate); ++i)
    if (set_mouse_sampling_rate(kbdc, rate[i]) != rate[i])
      return 0;

  if (get_mouse_status(kbdc, status, 0, 3) < 3)
    return 0;
  if ((status[1] == PSMD_RES_LOW) || (status[2] == rate[i - 1]))
    return 0;

  disable_aux_dev(kbdc);
  empty_aux_buffer(kbdc, 5);
  return 1;
}

static int enable_mmanplus(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  int data[3];

  if (!set_mouse_scaling(kbdc, 1))
    return 0;
  if (!mouse_ext_command(kbdc, 0x39) || !mouse_ext_command(kbdc, 0xdb))
    return 0;
  if (get_mouse_status(kbdc, data, 1, 3) < 3)
    return 0;

  if ((data[0] & MOUSE_PS2PLUS_SYNCMASK) != MOUSE_PS2PLUS_SYNC)
    return 0;
  if ((data[1] & 0xc3) != 0xc2)
    return 0;
  if (!MOUSE_PS2PLUS_CHECKBITS(data))
    return 0;
  if (MOUSE_PS2PLUS_PACKET_TYPE(data) != 0)
    return 0;

  if (arg == PROBE) {
    sc->hw.hwid &= 0x00ff;
    sc->hw.hwid |= data[2] << 8;
  }
  return 1;
}

static int enable_msintelli(struct psm_softc *sc, enum probearg arg);

static int enable_msexplorer(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  static uchar rate0[] = {200, 100, 80};
  static uchar rate1[] = {200, 200, 80};
  int id, i;

  enable_msintelli(sc, arg);

  for (i = 0; i < (int)sizeof(rate1); ++i)
    if (set_mouse_sampling_rate(kbdc, rate1[i]) != rate1[i])
      return 0;
  id = get_aux_id(kbdc);
  if (id != PSM_EXPLORER_ID)
    return 0;

  if (arg == PROBE) {
    sc->hw.buttons = 5;
    sc->hw.hwid = id;
  }

  for (i = 0; i < (int)sizeof(rate0); ++i)
    if (set_mouse_sampling_rate(kbdc, rate0[i]) != rate0[i])
      break;
  get_aux_id(kbdc);
  return 1;
}

static int enable_msintelli(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  static uchar rate[] = {200, 100, 80};
  int id, i;

  for (i = 0; i < (int)sizeof(rate); ++i)
    if (set_mouse_sampling_rate(kbdc, rate[i]) != rate[i])
      return 0;
  id = get_aux_id(kbdc);
  if (id != PSM_INTELLI_ID)
    return 0;

  if (arg == PROBE) {
    sc->hw.buttons = 3;
    sc->hw.hwid = id;
  }
  return 1;
}

// A4 Tech 4D Mouse
static int enable_4dmouse(struct psm_softc *sc, enum probearg arg) {
  static uchar rate[] = {200, 100, 80, 60, 40, 20};
  KBDC kbdc = sc->kbdc;
  int id, i;

  for (i = 0; i < (int)sizeof(rate); ++i)
    if (set_mouse_sampling_rate(kbdc, rate[i]) != rate[i])
      return 0;
  id = get_aux_id(kbdc);
  if (id != PSM_4DMOUSE_ID)
    return 0;

  if (arg == PROBE) {
    sc->hw.buttons = 3;
    sc->hw.hwid = id;
  }
  return 1;
}

static int enable_4dplus(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  int id;

  id = get_aux_id(kbdc);
  switch (id) {
  case PSM_4DPLUS_ID:
    break;
  case PSM_4DPLUS_RFSW35_ID:
    break;
  default:
    return 0;
  }

  if (arg == PROBE) {
    sc->hw.buttons = (id == PSM_4DPLUS_ID) ? 4 : 3;
    sc->hw.hwid = id;
  }
  return 1;
}

// SYNAPTICS

static void psm_sysctl_global(void) {
  const int W = FNU_CTLF_WRITE;

  sysctl_add_int("debug.psm.loglevel", &verbose, W, 0, 0);
  sysctl_add_int("debug.psm.hz", &psmhz, W, 0, 0);
  sysctl_add_int("debug.psm.errsecs", &psmerrsecs, W, 0, 0);
  sysctl_add_int("debug.psm.errusecs", &psmerrusecs, W, 0, 0);
  sysctl_add_int("debug.psm.secs", &psmsecs, W, 0, 0);
  sysctl_add_int("debug.psm.usecs", &psmusecs, W, 0, 0);
  sysctl_add_int("debug.psm.pkterrthresh", &pkterrthresh, W, 0, 0);
  sysctl_add_int("hw.psm.tap_enabled", &tap_enabled, W, 0, 0);
  sysctl_add_int("hw.psm.tap_threshold", &tap_threshold, W, 0, 0);
  sysctl_add_int("hw.psm.tap_timeout", &tap_timeout, W, 0, 0);
  sysctl_add_int("hw.psm.synaptics_support", &synaptics_support, 0, 0, 0);
  sysctl_add_int("hw.psm.trackpoint_support", &trackpoint_support, 0, 0, 0);
  sysctl_add_int("hw.psm.elantech_support", &elantech_support, 0, 0, 0);
  sysctl_add_int("hw.psm.mux_disabled", &mux_disabled, 0, 0, 0);
}

enum syn_range {
  SR_NONE, SR_RO, SR_PRESSURE, SR_WIDTH, SR_Y, SR_X, SR_QUEUE, SR_POSITIVE,
  SR_NONNEG, SR_XSYM, SR_YSYM, SR_BOOL
};

static void syn_node(struct psm_softc *sc, const char *name, int *var,
                     enum syn_range r) {
  int x = sc->synhw.maximumXCoord, y = sc->synhw.maximumYCoord;
  int lo = 0, hi = 0, flags = FNU_CTLF_WRITE;

  switch (r) {
  case SR_RO: flags = 0; break;
  case SR_PRESSURE: lo = 0; hi = 255; break;
  case SR_WIDTH: lo = 4; hi = 15; break;
  case SR_Y: lo = 0; hi = y; break;
  case SR_X: lo = 0; hi = x; break;
  case SR_QUEUE: lo = 1; hi = SYNAPTICS_PACKETQUEUE; break;
  case SR_POSITIVE: lo = 1; hi = 0x7fffffff; break;
  case SR_NONNEG: lo = 0; hi = 0x7fffffff; break;
  case SR_XSYM: lo = -x; hi = x; break;
  case SR_YSYM: lo = -y; hi = y; break;
  case SR_BOOL: lo = 0; hi = 1; break;
  case SR_NONE: break;
  }
  sysctl_add_int(name, var, flags, lo, hi);
}

static void synaptics_defaults(struct psm_softc *sc) {
  synapticsinfo_t *s = &sc->syninfo;

  if (sc->syninfo_set)
    return;
  sc->syninfo_set = 1;
  s->directional_scrolls = 0;
  s->max_x = 6143;
  s->max_y = 6143;
  if (sc->synhw.verticalScroll ||
      (sc->synhw.capMultiFinger == 0 && sc->synhw.capAdvancedGestures == 0))
    s->two_finger_scroll = 0;
  else
    s->two_finger_scroll = 1;
  s->min_pressure = 32;
  s->max_pressure = 220;
  s->max_width = 10;
  s->margin_top = 200;
  s->margin_right = 200;
  s->margin_bottom = 200;
  s->margin_left = 200;
  s->na_top = 1783;
  s->na_right = 563;
  s->na_bottom = 1408;
  s->na_left = 1600;
  s->window_min = 4;
  s->window_max = 10;
  s->multiplicator = 10000;
  s->weight_current = 3;
  s->weight_previous = 6;
  s->weight_previous_na = 20;
  s->weight_len_squared = 2000;
  s->div_min = 9;
  s->div_max = 17;
  s->div_max_na = 30;
  s->div_len = 100;
  s->tap_max_delta = 80;
  s->tap_min_queue = 2;
  s->taphold_timeout = tap_timeout;
  s->vscroll_hor_area = 0;
  s->vscroll_ver_area = -400 - s->margin_right;
  s->vscroll_min_delta = 50;
  s->vscroll_div_min = 100;
  s->vscroll_div_max = 150;
  s->touchpad_off = 0;
  s->three_finger_drag = 0;
  s->natural_scroll = 0;
  s->softbuttons_y = 0;
  s->softbutton2_x = 0;
  s->softbutton3_x = 0;
  if (sc->synhw.capClickPad) {
    s->softbuttons_y = 1700;
    if (sc->synhw.topButtonPad)
      s->softbuttons_y = -1700;
    s->softbutton2_x = 3100;
    s->softbutton3_x = 3900;
  }
}

static void synaptics_sysctl_create_tree(struct psm_softc *sc) {
  synapticsinfo_t *s = &sc->syninfo;

  syn_node(sc, "hw.psm.synaptics.directional_scrolls", &s->directional_scrolls, SR_NONE);
  syn_node(sc, "hw.psm.synaptics.max_x", &s->max_x, SR_RO);
  syn_node(sc, "hw.psm.synaptics.max_y", &s->max_y, SR_RO);
  syn_node(sc, "hw.psm.synaptics.two_finger_scroll", &s->two_finger_scroll, SR_NONE);
  syn_node(sc, "hw.psm.synaptics.min_pressure", &s->min_pressure, SR_PRESSURE);
  syn_node(sc, "hw.psm.synaptics.max_pressure", &s->max_pressure, SR_PRESSURE);
  syn_node(sc, "hw.psm.synaptics.max_width", &s->max_width, SR_WIDTH);
  syn_node(sc, "hw.psm.synaptics.margin_top", &s->margin_top, SR_Y);
  syn_node(sc, "hw.psm.synaptics.margin_right", &s->margin_right, SR_X);
  syn_node(sc, "hw.psm.synaptics.margin_bottom", &s->margin_bottom, SR_Y);
  syn_node(sc, "hw.psm.synaptics.margin_left", &s->margin_left, SR_X);
  syn_node(sc, "hw.psm.synaptics.na_top", &s->na_top, SR_Y);
  syn_node(sc, "hw.psm.synaptics.na_right", &s->na_right, SR_X);
  syn_node(sc, "hw.psm.synaptics.na_bottom", &s->na_bottom, SR_Y);
  syn_node(sc, "hw.psm.synaptics.na_left", &s->na_left, SR_X);
  syn_node(sc, "hw.psm.synaptics.window_min", &s->window_min, SR_QUEUE);
  syn_node(sc, "hw.psm.synaptics.window_max", &s->window_max, SR_QUEUE);
  syn_node(sc, "hw.psm.synaptics.multiplicator", &s->multiplicator, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.weight_current", &s->weight_current, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.weight_previous", &s->weight_previous, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.weight_previous_na", &s->weight_previous_na, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.weight_len_squared", &s->weight_len_squared, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.div_min", &s->div_min, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.div_max", &s->div_max, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.div_max_na", &s->div_max_na, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.div_len", &s->div_len, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.tap_max_delta", &s->tap_max_delta, SR_NONNEG);
  syn_node(sc, "hw.psm.synaptics.tap_min_queue", &s->tap_min_queue, SR_QUEUE);
  syn_node(sc, "hw.psm.synaptics.taphold_timeout", &s->taphold_timeout, SR_NONNEG);
  syn_node(sc, "hw.psm.synaptics.vscroll_hor_area", &s->vscroll_hor_area, SR_XSYM);
  syn_node(sc, "hw.psm.synaptics.vscroll_ver_area", &s->vscroll_ver_area, SR_YSYM);
  syn_node(sc, "hw.psm.synaptics.vscroll_min_delta", &s->vscroll_min_delta, SR_NONNEG);
  syn_node(sc, "hw.psm.synaptics.vscroll_div_min", &s->vscroll_div_min, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.vscroll_div_max", &s->vscroll_div_max, SR_POSITIVE);
  syn_node(sc, "hw.psm.synaptics.touchpad_off", &s->touchpad_off, SR_BOOL);
  syn_node(sc, "hw.psm.synaptics.three_finger_drag", &s->three_finger_drag, SR_BOOL);
  syn_node(sc, "hw.psm.synaptics.natural_scroll", &s->natural_scroll, SR_BOOL);
  if (sc->synhw.capClickPad) {
    syn_node(sc, "hw.psm.synaptics.softbuttons_y", &s->softbuttons_y, SR_YSYM);
    syn_node(sc, "hw.psm.synaptics.softbutton2_x", &s->softbutton2_x, SR_X);
    syn_node(sc, "hw.psm.synaptics.softbutton3_x", &s->softbutton3_x, SR_X);
  }
}

static int synaptics_preferred_mode(struct psm_softc *sc) {
  int mode_byte;

  if (sc->hw.model != FNU_MOUSE_MODEL_SYNAPTICS) {
    if (tap_enabled == 0)
      return 0x04;
    else
      return 0x00;
  }

  mode_byte = 0xc4;
  if (sc->synhw.capExtended)
    mode_byte |= 1;
  return mode_byte;
}

static void synaptics_set_mode(struct psm_softc *sc, int mode_byte) {
  mouse_ext_command(sc->kbdc, mode_byte);
  set_mouse_sampling_rate(sc->kbdc, 20);

  if ((sc->synhw.capAdvancedGestures || sc->synhw.capReportsV) &&
      sc->hw.model == FNU_MOUSE_MODEL_SYNAPTICS && !(mode_byte & (1 << 5))) {
    mouse_ext_command(sc->kbdc, SYNAPTICS_READ_MODEL_ID);
    set_mouse_sampling_rate(sc->kbdc, 0xc8);
  }
}

static int enable_synaptics_mux(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  int port, version, probe = 0, active_ports_count = 0, active_ports_mask = 0;

  sc->muxsinglesyna = 0;

  if (mux_disabled == 1)
    return 0;

  version = enable_aux_mux(kbdc);
  if (version == -1)
    return 0;

  for (port = 0; port < KBDC_AUX_MUX_NUM_PORTS; port++) {
    if (verbose >= 3)
      printf("aux_mux: ping port %d\n", port);
    set_active_aux_mux_port(kbdc, port);
    if (enable_aux_dev(kbdc) && disable_aux_dev(kbdc)) {
      active_ports_count++;
      active_ports_mask |= 1 << port;
    }
  }

  if (verbose >= 2)
    printf("Active Multiplexing PS/2 controller v%d.%d with %d active port(s)\n",
            version >> 4 & 0x0f, version & 0x0f, active_ports_count);

  for (port = 0; port < KBDC_AUX_MUX_NUM_PORTS; port++) {
    if ((active_ports_mask & 1 << port) == 0)
      continue;
    if (verbose >= 3)
      printf("aux_mux: probe port %d\n", port);
    set_active_aux_mux_port(kbdc, port);
    probe = enable_synaptics(sc, arg);
    if (probe) {
      if (arg == PROBE)
        sc->muxport = port;
      break;
    }
  }

  disable_aux_mux(kbdc);

  if (!enable_aux_dev(kbdc) || !disable_aux_dev(kbdc)) {
    reset_kbd(kbdc);
    if (!enable_aux_dev(kbdc) || !disable_aux_dev(kbdc))
      printf("psm: AUX MUX hang detected!\n");
  }
  empty_both_buffers(kbdc, 10);

  if (active_ports_count == 1)
    sc->muxsinglesyna = probe;
  return active_ports_count != 1 ? probe : 0;
}

static int enable_single_synaptics_mux(struct psm_softc *sc, enum probearg arg __unused) {
  return sc->muxsinglesyna;
}

static int enable_trackpoint(struct psm_softc *sc, enum probearg arg);

static int enable_synaptics(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  struct fnu_synapticshw synhw;
  int status[3], buttons;

  if (verbose >= 3)
    printf("synaptics: BEGIN init\n");

  set_mouse_scaling(kbdc, 1);

  if (mouse_ext_command(kbdc, SYNAPTICS_READ_IDENTITY) == 0)
    return 0;
  if (get_mouse_status(kbdc, status, 0, 3) != 3)
    return 0;
  if (status[1] != 0x47)
    return 0;

  memset(&synhw, 0, sizeof(synhw));
  synhw.infoMinor = status[0];
  synhw.infoMajor = status[2] & 0x0f;

  if (verbose >= 2)
    printf("Synaptics Touchpad v%d.%d\n", synhw.infoMajor, synhw.infoMinor);

  if (synhw.infoMajor < 4) {
    printf("  Unsupported (pre-v4) Touchpad detected\n");
    return 0;
  }

  if (mouse_ext_command(kbdc, SYNAPTICS_READ_MODEL_ID) == 0)
    return 0;
  if (get_mouse_status(kbdc, status, 0, 3) != 3)
    return 0;
  if ((status[1] & 0x01) != 0) {
    printf("  Failed to read model information\n");
    return 0;
  }

  synhw.infoRot180 = (status[0] & 0x80) != 0;
  synhw.infoPortrait = (status[0] & 0x40) != 0;
  synhw.infoSensor = status[0] & 0x3f;
  synhw.infoHardware = (status[1] & 0xfe) >> 1;
  synhw.infoNewAbs = (status[2] & 0x80) != 0;
  synhw.capPen = (status[2] & 0x40) != 0;
  synhw.infoSimplC = (status[2] & 0x20) != 0;
  synhw.infoGeometry = status[2] & 0x0f;

  if (verbose >= 2) {
    printf("  Model information:\n");
    printf("   infoRot180: %d\n", synhw.infoRot180);
    printf("   infoPortrait: %d\n", synhw.infoPortrait);
    printf("   infoSensor: %d\n", synhw.infoSensor);
    printf("   infoHardware: %d\n", synhw.infoHardware);
    printf("   infoNewAbs: %d\n", synhw.infoNewAbs);
    printf("   capPen: %d\n", synhw.capPen);
    printf("   infoSimplC: %d\n", synhw.infoSimplC);
    printf("   infoGeometry: %d\n", synhw.infoGeometry);
  }

  synhw.maximumXCoord = 5472;
  synhw.maximumYCoord = 4448;
  synhw.minimumXCoord = 1472;
  synhw.minimumYCoord = 1408;

  if (mouse_ext_command(kbdc, SYNAPTICS_READ_CAPABILITIES) == 0)
    return 0;
  if (get_mouse_status(kbdc, status, 0, 3) != 3)
    return 0;
  if (!SYNAPTICS_VERSION_GE(synhw, 7, 5) && status[1] != 0x47) {
    printf("  Failed to read extended capability bits\n");
    return 0;
  }

  buttons = 0;
  synhw.capExtended = (status[0] & 0x80) != 0;
  if (synhw.capExtended) {
    synhw.nExtendedQueries = (status[0] & 0x70) >> 4;
    synhw.capMiddle = (status[0] & 0x04) != 0;
    synhw.capPassthrough = (status[2] & 0x80) != 0;
    synhw.capLowPower = (status[2] & 0x40) != 0;
    synhw.capMultiFingerReport = (status[2] & 0x20) != 0;
    synhw.capSleep = (status[2] & 0x10) != 0;
    synhw.capFourButtons = (status[2] & 0x08) != 0;
    synhw.capBallistics = (status[2] & 0x04) != 0;
    synhw.capMultiFinger = (status[2] & 0x02) != 0;
    synhw.capPalmDetect = (status[2] & 0x01) != 0;

    if (!set_mouse_scaling(kbdc, 1))
      return 0;
    if (mouse_ext_command(kbdc, SYNAPTICS_READ_RESOLUTIONS) == 0)
      return 0;
    if (get_mouse_status(kbdc, status, 0, 3) != 3)
      return 0;

    if (status[0] != 0 && (status[1] & 0x80) && status[2] != 0) {
      synhw.infoXupmm = status[0];
      synhw.infoYupmm = status[2];
    }

    if (verbose >= 2) {
      printf("  Extended capabilities:\n");
      printf("   capExtended: %d\n", synhw.capExtended);
      printf("   capMiddle: %d\n", synhw.capMiddle);
      printf("   nExtendedQueries: %d\n", synhw.nExtendedQueries);
      printf("   capPassthrough: %d\n", synhw.capPassthrough);
      printf("   capLowPower: %d\n", synhw.capLowPower);
      printf("   capMultiFingerReport: %d\n", synhw.capMultiFingerReport);
      printf("   capSleep: %d\n", synhw.capSleep);
      printf("   capFourButtons: %d\n", synhw.capFourButtons);
      printf("   capBallistics: %d\n", synhw.capBallistics);
      printf("   capMultiFinger: %d\n", synhw.capMultiFinger);
      printf("   capPalmDetect: %d\n", synhw.capPalmDetect);
      printf("   infoXupmm: %d\n", synhw.infoXupmm);
      printf("   infoYupmm: %d\n", synhw.infoYupmm);
    }

    if (synhw.nExtendedQueries >= 1) {
      if (!set_mouse_scaling(kbdc, 1))
        return 0;
      if (mouse_ext_command(kbdc, SYNAPTICS_READ_EXTENDED) == 0)
        return 0;
      if (get_mouse_status(kbdc, status, 0, 3) != 3)
        return 0;
      synhw.verticalScroll = (status[0] & 0x01) != 0;
      synhw.horizontalScroll = (status[0] & 0x02) != 0;
      synhw.verticalWheel = (status[0] & 0x08) != 0;
      synhw.nExtendedButtons = (status[1] & 0xf0) >> 4;
      synhw.capEWmode = (status[0] & 0x04) != 0;
      if (verbose >= 2) {
        printf("  Extended model ID:\n");
        printf("   verticalScroll: %d\n", synhw.verticalScroll);
        printf("   horizontalScroll: %d\n", synhw.horizontalScroll);
        printf("   verticalWheel: %d\n", synhw.verticalWheel);
        printf("   nExtendedButtons: %d\n", synhw.nExtendedButtons);
        printf("   capEWmode: %d\n", synhw.capEWmode);
      }
      buttons = synhw.nExtendedButtons + synhw.capMiddle;
    } else
      buttons = synhw.capFourButtons ? 1 : 0;

    if (synhw.nExtendedQueries >= 4) {
      if (!set_mouse_scaling(kbdc, 1))
        return 0;
      if (mouse_ext_command(kbdc, SYNAPTICS_READ_CAPABILITIES_CONT) == 0)
        return 0;
      if (get_mouse_status(kbdc, status, 0, 3) != 3)
        return 0;

      synhw.capClickPad = (status[1] & 0x01) << 1;
      synhw.capClickPad |= (status[0] & 0x10) != 0;
      synhw.capDeluxeLEDs = (status[1] & 0x02) != 0;
      synhw.noAbsoluteFilter = (status[1] & 0x04) != 0;
      synhw.capReportsV = (status[1] & 0x08) != 0;
      synhw.capUniformClickPad = (status[1] & 0x10) != 0;
      synhw.capReportsMin = (status[1] & 0x20) != 0;
      synhw.capInterTouch = (status[1] & 0x40) != 0;
      synhw.capReportsMax = (status[0] & 0x02) != 0;
      synhw.capClearPad = (status[0] & 0x04) != 0;
      synhw.capAdvancedGestures = (status[0] & 0x08) != 0;
      synhw.capCoveredPad = (status[0] & 0x80) != 0;

      if (synhw.capReportsMax) {
        if (!set_mouse_scaling(kbdc, 1))
          return 0;
        if (mouse_ext_command(kbdc, SYNAPTICS_READ_MAX_COORDS) == 0)
          return 0;
        if (get_mouse_status(kbdc, status, 0, 3) != 3)
          return 0;

        synhw.maximumXCoord = (status[0] << 5) | ((status[1] & 0x0f) << 1);
        synhw.maximumYCoord = (status[2] << 5) | ((status[1] & 0xf0) >> 3);
      }

      if (synhw.capReportsMin) {
        if (!set_mouse_scaling(kbdc, 1))
          return 0;
        if (mouse_ext_command(kbdc, SYNAPTICS_READ_MIN_COORDS) == 0)
          return 0;
        if (get_mouse_status(kbdc, status, 0, 3) != 3)
          return 0;

        synhw.minimumXCoord = (status[0] << 5) | ((status[1] & 0x0f) << 1);
        synhw.minimumYCoord = (status[2] << 5) | ((status[1] & 0xf0) >> 3);
      }

      // the PnP id, not PS/2, says what kind of clickpad
      if (synhw.capClickPad && pnp_type == PSMCPNP_FORCEPAD)
        synhw.forcePad = 1;
      else if (synhw.capClickPad && pnp_type == PSMCPNP_TOPBUTTONPAD)
        synhw.topButtonPad = 1;

      if (verbose >= 2) {
        printf("  Continued capabilities:\n");
        printf("   capClickPad: %d\n", synhw.capClickPad);
        printf("   capDeluxeLEDs: %d\n", synhw.capDeluxeLEDs);
        printf("   noAbsoluteFilter: %d\n", synhw.noAbsoluteFilter);
        printf("   capReportsV: %d\n", synhw.capReportsV);
        printf("   capUniformClickPad: %d\n", synhw.capUniformClickPad);
        printf("   capReportsMin: %d\n", synhw.capReportsMin);
        printf("   capInterTouch: %d\n", synhw.capInterTouch);
        printf("   capReportsMax: %d\n", synhw.capReportsMax);
        printf("   capClearPad: %d\n", synhw.capClearPad);
        printf("   capAdvancedGestures: %d\n", synhw.capAdvancedGestures);
        printf("   capCoveredPad: %d\n", synhw.capCoveredPad);
        if (synhw.capReportsMax) {
          printf("   maximumXCoord: %d\n", synhw.maximumXCoord);
          printf("   maximumYCoord: %d\n", synhw.maximumYCoord);
        }
        if (synhw.capReportsMin) {
          printf("   minimumXCoord: %d\n", synhw.minimumXCoord);
          printf("   minimumYCoord: %d\n", synhw.minimumYCoord);
        }
      }
      buttons += synhw.capClickPad;
    }
  }

  if (verbose >= 2) {
    if (synhw.capExtended)
      printf("  Additional Buttons: %d\n", buttons);
    else
      printf("  No extended capabilities\n");
  }

  buttons += 3;

  if (mouse_ext_command(kbdc, SYNAPTICS_READ_MODES) == 0)
    return 0;
  if (get_mouse_status(kbdc, status, 0, 3) != 3)
    return 0;
  if (!SYNAPTICS_VERSION_GE(synhw, 7, 5) && status[1] != 0x47) {
    printf("  Failed to read mode byte\n");
    return 0;
  }

  if (arg == PROBE)
    sc->synhw = synhw;
  if (!synaptics_support)
    return 0;

  sc->hw.model = FNU_MOUSE_MODEL_SYNAPTICS;

  synaptics_set_mode(sc, synaptics_preferred_mode(sc));

  if (trackpoint_support && synhw.capPassthrough)
    enable_trackpoint(sc, arg);

  if (verbose >= 3)
    printf("synaptics: END init (%d buttons)\n", buttons);

  if (arg == PROBE) {
    synaptics_defaults(sc);
    sc->hw.buttons = buttons;
  }
  return 1;
}

static void synaptics_passthrough_on(struct psm_softc *sc) {
  if (verbose >= 2)
    printf("psm: setting pass-through mode.\n");
  synaptics_set_mode(sc, synaptics_preferred_mode(sc) | (1 << 5));
}

static void synaptics_passthrough_off(struct psm_softc *sc) {
  if (verbose >= 2)
    printf("psm: turning pass-through mode off.\n");
  set_mouse_scaling(sc->kbdc, 2);
  set_mouse_scaling(sc->kbdc, 1);
  synaptics_set_mode(sc, synaptics_preferred_mode(sc));
}

// TRACKPOINT

static int trackpoint_command(struct psm_softc *sc, int cmd, int loc, int val) {
  const int seq[] = {0xe2, cmd, loc, val};
  int i;

  if (sc->synhw.capPassthrough)
    synaptics_passthrough_on(sc);

  for (i = 0; i < 4; i++) {
    if (sc->synhw.capPassthrough && (seq[i] == 0xff || seq[i] == 0xe7))
      if (send_aux_command(sc->kbdc, 0xe7) != PSM_ACK) {
        synaptics_passthrough_off(sc);
        return -1;
      }
    if (send_aux_command(sc->kbdc, seq[i]) != PSM_ACK) {
      if (sc->synhw.capPassthrough)
        synaptics_passthrough_off(sc);
      return -1;
    }
  }

  if (sc->synhw.capPassthrough)
    synaptics_passthrough_off(sc);
  return 0;
}

static struct {
  const char *name;
  int *var;
  int mask, loc;
} tp_node[12];

static int trackpoint_write(void *arg, int old __unused) {
  int i = (int)(intptr_t)arg, error;

  if (!kbdc_lock(psm_sc.kbdc, 1))
    return FNU_ERR_BUSY;
  error = trackpoint_command(&psm_sc, tp_node[i].mask == 0 ? 0x81 : 0x47,
                             tp_node[i].loc,
                             tp_node[i].mask == 0 ? *tp_node[i].var : tp_node[i].mask);
  kbdc_lock(psm_sc.kbdc, 0);
  return error != 0 ? FNU_ERR_IO : 0;
}

static void tp_add(struct psm_softc *sc __unused, int i, const char *name, int *var,
                   int mask, int loc) {
  tp_node[i].name = name;
  tp_node[i].var = var;
  tp_node[i].mask = mask;
  tp_node[i].loc = loc;
  if (sysctl_add_int(name, var, FNU_CTLF_WRITE, 0, mask == 0 ? 255 : 1) == 0)
    sysctl_on_write(name, trackpoint_write, (void *)(intptr_t)i);
}

// power-on values
#define TP_DEF_SENS 0x80
#define TP_DEF_INERTIA 0x06
#define TP_DEF_SPEED 0x61
#define TP_DEF_REACH 0x0a
#define TP_DEF_DRAGHYS 0xff
#define TP_DEF_MINDRAG 0x14
#define TP_DEF_UP_THRESH 0xff
#define TP_DEF_THRESH 0x08
#define TP_DEF_JENKS 0x87
#define TP_DEF_Z_TIME 0x26

static void trackpoint_defaults(struct psm_softc *sc) {
  sc->tpinfo.sensitivity = TP_DEF_SENS;
  sc->tpinfo.inertia = TP_DEF_INERTIA;
  sc->tpinfo.uplateau = TP_DEF_SPEED;
  sc->tpinfo.reach = TP_DEF_REACH;
  sc->tpinfo.draghys = TP_DEF_DRAGHYS;
  sc->tpinfo.mindrag = TP_DEF_MINDRAG;
  sc->tpinfo.upthresh = TP_DEF_UP_THRESH;
  sc->tpinfo.threshold = TP_DEF_THRESH;
  sc->tpinfo.jenks = TP_DEF_JENKS;
  sc->tpinfo.ztime = TP_DEF_Z_TIME;
  sc->tpinfo.pts = 0x00;
  sc->tpinfo.skipback = 0x00;
}

static void trackpoint_sysctl_create_tree(struct psm_softc *sc) {
  tp_add(sc, 0, "hw.psm.trackpoint.sensitivity", &sc->tpinfo.sensitivity, 0, 0x4a);
  tp_add(sc, 1, "hw.psm.trackpoint.negative_inertia", &sc->tpinfo.inertia, 0, 0x4d);
  tp_add(sc, 2, "hw.psm.trackpoint.upper_plateau", &sc->tpinfo.uplateau, 0, 0x60);
  tp_add(sc, 3, "hw.psm.trackpoint.backup_range", &sc->tpinfo.reach, 0, 0x57);
  tp_add(sc, 4, "hw.psm.trackpoint.drag_hysteresis", &sc->tpinfo.draghys, 0, 0x58);
  tp_add(sc, 5, "hw.psm.trackpoint.minimum_drag", &sc->tpinfo.mindrag, 0, 0x59);
  tp_add(sc, 6, "hw.psm.trackpoint.up_threshold", &sc->tpinfo.upthresh, 0, 0x5a);
  tp_add(sc, 7, "hw.psm.trackpoint.threshold", &sc->tpinfo.threshold, 0, 0x5c);
  tp_add(sc, 8, "hw.psm.trackpoint.jenks_curvature", &sc->tpinfo.jenks, 0, 0x5d);
  tp_add(sc, 9, "hw.psm.trackpoint.z_time", &sc->tpinfo.ztime, 0, 0x5e);
  tp_add(sc, 10, "hw.psm.trackpoint.press_to_select", &sc->tpinfo.pts, 1, 0x2c);
  tp_add(sc, 11, "hw.psm.trackpoint.skip_backups", &sc->tpinfo.skipback, 8, 0x2d);
}

// power-on reset: E2 7F, AA 00 back
static int trackpoint_reset(struct psm_softc *sc) {
  int a = -1, b = -1;

  if (sc->synhw.capPassthrough)
    synaptics_passthrough_on(sc);
  if (send_aux_command(sc->kbdc, 0xe2) == PSM_ACK &&
      send_aux_command(sc->kbdc, 0x7f) == PSM_ACK && (a = read_aux_data(sc->kbdc)) == 0xaa)
    b = read_aux_data(sc->kbdc);
  if (sc->synhw.capPassthrough)
    synaptics_passthrough_off(sc);
  return a == 0xaa && b == 0x00 ? 0 : -1;
}

// after a power-on reset only what differs from the power-on values
static void set_trackpoint_parameters(struct psm_softc *sc, int por) {
#define TP_SET(loc, v, d)                                                                    \
  do {                                                                                       \
    if (!por || (v) != (d))                                                                  \
      trackpoint_command(sc, 0x81, loc, v);                                                  \
  } while (0)
  TP_SET(0x4a, sc->tpinfo.sensitivity, TP_DEF_SENS);
  TP_SET(0x60, sc->tpinfo.uplateau, TP_DEF_SPEED);
  TP_SET(0x4d, sc->tpinfo.inertia, TP_DEF_INERTIA);
  TP_SET(0x57, sc->tpinfo.reach, TP_DEF_REACH);
  TP_SET(0x58, sc->tpinfo.draghys, TP_DEF_DRAGHYS);
  TP_SET(0x59, sc->tpinfo.mindrag, TP_DEF_MINDRAG);
  TP_SET(0x5a, sc->tpinfo.upthresh, TP_DEF_UP_THRESH);
  TP_SET(0x5c, sc->tpinfo.threshold, TP_DEF_THRESH);
  TP_SET(0x5d, sc->tpinfo.jenks, TP_DEF_JENKS);
  TP_SET(0x5e, sc->tpinfo.ztime, TP_DEF_Z_TIME);
#undef TP_SET
  if (sc->tpinfo.pts == 0x01)
    trackpoint_command(sc, 0x47, 0x2c, 0x01);
  if (sc->tpinfo.skipback == 0x01)
    trackpoint_command(sc, 0x47, 0x2d, 0x08);
}

static int enable_trackpoint(struct psm_softc *sc, enum probearg arg) {
  KBDC kbdc = sc->kbdc;
  int vendor, firmware, por;

  if (sc->synhw.capPassthrough)
    synaptics_passthrough_on(sc);

  if (send_aux_command(kbdc, 0xe1) != PSM_ACK)
    goto no_trackpoint;
  vendor = read_aux_data(kbdc);
  if (vendor <= 0 || vendor >= TRACKPOINT_VENDOR_UNKNOWN)
    goto no_trackpoint;
  firmware = read_aux_data(kbdc);
  if (firmware < 0x01)
    goto no_trackpoint;
  if (!trackpoint_support)
    goto no_trackpoint;

  if (sc->synhw.capPassthrough)
    synaptics_passthrough_off(sc);

  if (arg == PROBE) {
    trackpoint_defaults(sc);
    sc->tptree = 1;
    if (!sc->synhw.capPassthrough) {
      sc->hw.hwid = firmware;
      sc->hw.buttons = 3;
    }
    if (verbose >= 2)
      printf("psm: Trackpoint v=%x f=%x\n", vendor, firmware);
    sc->tpinfo.vendor = vendor;
    sc->tpinfo.firmware = firmware;
  }

  por = vendor == TRACKPOINT_VENDOR_IBM && trackpoint_reset(sc) == 0;
  set_trackpoint_parameters(sc, por);
  return 1;

no_trackpoint:
  if (sc->synhw.capPassthrough)
    synaptics_passthrough_off(sc);
  return 0;
}

// Interlink electronics VersaPad
static int enable_versapad(struct psm_softc *sc, enum probearg arg __unused) {
  KBDC kbdc = sc->kbdc;
  int data[3];

  set_mouse_resolution(kbdc, PSMD_RES_MEDIUM_HIGH);
  set_mouse_sampling_rate(kbdc, 100);
  set_mouse_scaling(kbdc, 1);
  set_mouse_scaling(kbdc, 1);
  set_mouse_scaling(kbdc, 1);
  set_mouse_scaling(kbdc, 1);
  if (get_mouse_status(kbdc, data, 0, 3) < 3)
    return 0;
  if (data[2] != 0xa || data[1] != 0)
    return 0;
  set_mouse_scaling(kbdc, 1);
  return 1;
}

// ELANTECH

static int elantech_read_1(KBDC kbdc, int hwversion, int reg, int *val) {
  int res, readcmd, retidx, resp[3];

  readcmd = hwversion == 2 ? ELANTECH_REG_READ : ELANTECH_REG_RDWR;
  retidx = hwversion == 4 ? 1 : 0;

  res = send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
  res |= send_aux_command(kbdc, readcmd) != PSM_ACK;
  res |= send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
  res |= send_aux_command(kbdc, reg) != PSM_ACK;
  res |= get_mouse_status(kbdc, resp, 0, 3) != 3;

  if (res == 0)
    *val = resp[retidx];
  return res;
}

static int elantech_write_1(KBDC kbdc, int hwversion, int reg, int val) {
  int res, writecmd;

  writecmd = hwversion == 2 ? ELANTECH_REG_WRITE : ELANTECH_REG_RDWR;

  res = send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
  res |= send_aux_command(kbdc, writecmd) != PSM_ACK;
  res |= send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
  res |= send_aux_command(kbdc, reg) != PSM_ACK;
  if (hwversion == 4) {
    res |= send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
    res |= send_aux_command(kbdc, writecmd) != PSM_ACK;
  }
  res |= send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
  res |= send_aux_command(kbdc, val) != PSM_ACK;
  res |= set_mouse_scaling(kbdc, 1) == 0;
  return res;
}

static int elantech_cmd(KBDC kbdc, int hwversion, int cmd, int *resp) {
  int res;

  if (hwversion == 2) {
    res = set_mouse_scaling(kbdc, 1) == 0;
    res |= mouse_ext_command(kbdc, cmd) == 0;
  } else {
    res = send_aux_command(kbdc, ELANTECH_CUSTOM_CMD) != PSM_ACK;
    res |= send_aux_command(kbdc, cmd) != PSM_ACK;
  }
  res |= get_mouse_status(kbdc, resp, 0, 3) != 3;
  return res;
}

static int elantech_init(KBDC kbdc, elantechhw_t *elanhw) {
  int i, val, res, hwversion, reg10;

  hwversion = elanhw->hwversion;
  reg10 = -1;
  switch (hwversion) {
  case 2:
    reg10 = elanhw->fwversion == 0x020030 ? 0x54 : 0xc4;
    res = elantech_write_1(kbdc, hwversion, 0x10, reg10);
    if (res)
      break;
    res = elantech_write_1(kbdc, hwversion, 0x11, 0x8A);
    break;
  case 3:
    reg10 = 0x0b;
    res = elantech_write_1(kbdc, hwversion, 0x10, reg10);
    break;
  case 4:
    res = elantech_write_1(kbdc, hwversion, 0x07, 0x01);
    break;
  default:
    res = 1;
  }

  if (res == 0 && reg10 >= 0) {
    for (i = 0; i < 5; i++) {
      if (elantech_read_1(kbdc, hwversion, 0x10, &val) == 0)
        break;
      delay(2000);
    }
    if (i == 5)
      res = 1;
  }

  if (res)
    printf("psm: couldn't set absolute mode\n");
  return res;
}

static void elantech_init_synaptics(struct psm_softc *sc) {
  sc->synhw.infoMajor = sc->elanhw.hwversion;
  sc->synhw.infoMinor = sc->elanhw.fwversion;
  sc->synhw.infoXupmm = sc->elanhw.dpmmx;
  sc->synhw.infoYupmm = sc->elanhw.dpmmy;
  sc->synhw.verticalScroll = 0;
  sc->synhw.nExtendedQueries = 4;
  sc->synhw.capExtended = 1;
  sc->synhw.capPassthrough = sc->elanhw.hastrackpoint;
  sc->synhw.capClickPad = sc->elanhw.isclickpad;
  sc->synhw.capMultiFinger = 1;
  if (sc->elanhw.issemimt)
    sc->synhw.capAdvancedGestures = 1;
  else
    sc->synhw.capReportsV = 1;
  sc->synhw.capPalmDetect = 1;
  sc->synhw.capPen = 0;
  sc->synhw.capReportsMax = 1;
  sc->synhw.maximumXCoord = sc->elanhw.sizex;
  sc->synhw.maximumYCoord = sc->elanhw.sizey;
  sc->synhw.capReportsMin = 1;
  sc->synhw.minimumXCoord = 0;
  sc->synhw.minimumYCoord = 0;

  if (!sc->syninfo_set) {
    synaptics_defaults(sc);

    sc->syninfo.max_x = sc->elanhw.sizex;
    sc->syninfo.max_y = sc->elanhw.sizey;
    sc->syninfo.min_pressure = 1;
    sc->syninfo.max_width = 7;
    sc->syninfo.tap_min_queue = 1;
    sc->syninfo.margin_top = 0;
    sc->syninfo.margin_right = 0;
    sc->syninfo.margin_bottom = 0;
    sc->syninfo.margin_left = 0;
    sc->syninfo.na_top = 0;
    sc->syninfo.na_right = 0;
    sc->syninfo.na_bottom = 0;
    sc->syninfo.na_left = 0;
    sc->syninfo.weight_len_squared = 200;
    sc->syninfo.div_min = 3;
    sc->syninfo.div_max = 6;
    sc->syninfo.div_max_na = 10;
    sc->syninfo.div_len = 30;
    sc->syninfo.tap_max_delta = 25;
    sc->syninfo.vscroll_hor_area = 0;
    sc->syninfo.vscroll_ver_area = 0;
    sc->syninfo.vscroll_min_delta = 15;
    sc->syninfo.vscroll_div_min = 30;
    sc->syninfo.vscroll_div_max = 50;
    if (sc->elanhw.isclickpad) {
      sc->syninfo.softbuttons_y = sc->elanhw.sizey / 4;
      sc->syninfo.softbutton2_x = sc->elanhw.sizex * 11 / 25;
      sc->syninfo.softbutton3_x = sc->elanhw.sizex * 14 / 25;
    }
  }
}

static int enable_elantech(struct psm_softc *sc, enum probearg arg) {
  static const int ic2hw[] = {0, 0, 2, 0, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
  static const int fw_sizes[][3] = {
      {0x020030, 1152, 768}, {0x020800, 1152, 768}, {0x020b00, 1152, 768},
      {0x040215, 900, 500},  {0x040216, 819, 405},  {0x040219, 900, 500},
  };
  elantechhw_t elanhw;
  int icversion, hwversion, xtr, i, id, resp[3], dpix, dpiy;
  KBDC kbdc = sc->kbdc;

  if (verbose >= 3)
    printf("elantech: BEGIN init\n");

  set_mouse_scaling(kbdc, 1);
  set_mouse_scaling(kbdc, 1);
  set_mouse_scaling(kbdc, 1);
  if (get_mouse_status(kbdc, resp, 0, 3) != 3)
    return 0;

  if (!ELANTECH_MAGIC(resp))
    return 0;

  if (elantech_cmd(kbdc, 2, ELANTECH_FW_VERSION, resp))
    return 0;

  memset(&elanhw, 0, sizeof(elanhw));

  elanhw.fwversion = (resp[0] << 16) | (resp[1] << 8) | resp[2];
  icversion = resp[0] & 0x0f;
  hwversion = ic2hw[icversion];

  if (verbose >= 2)
    printf("Elantech touchpad hardware v.%d firmware v.%x\n", hwversion, elanhw.fwversion);

  if (ELANTECH_HW_IS_V1(elanhw.fwversion)) {
    printf("  Unsupported touchpad hardware (v1)\n");
    return 0;
  }
  if (hwversion == 0) {
    printf("  Unknown touchpad hardware (firmware v.%x)\n", elanhw.fwversion);
    return 0;
  }

  elanhw.hwversion = hwversion;
  elanhw.issemimt = hwversion == 2;
  elanhw.isclickpad = (resp[1] & 0x10) != 0;
  elanhw.hassmbusnotify = icversion == 0x0f && (resp[1] & 0x20) != 0 && resp[2] != 0;
  elanhw.has3buttons = elanhw.hassmbusnotify;
  elanhw.hascrc = (resp[1] & 0x40) != 0;
  elanhw.haspressure = elanhw.fwversion >= 0x020800;

  if (elantech_cmd(kbdc, hwversion, ELANTECH_CAPABILITIES, resp) != 0) {
    printf("  Failed to read capability bits\n");
    return 0;
  }

  elanhw.ntracesx = imax(resp[1], 3);
  elanhw.ntracesy = imax(resp[2], 3);
  elanhw.hastrackpoint = (resp[0] & 0x80) != 0;

  switch (hwversion) {
  case 4:
    if (elantech_cmd(kbdc, hwversion, ELANTECH_RESOLUTION, resp) == 0) {
      dpix = (resp[1] & 0x0f) * 10 + 790;
      dpiy = ((resp[1] & 0xf0) >> 4) * 10 + 790;
      elanhw.dpmmx = (dpix * 10 + 5) / 254;
      elanhw.dpmmy = (dpiy * 10 + 5) / 254;
      break;
    }
    // fallthrough
  case 2:
  case 3:
    elanhw.dpmmx = elanhw.dpmmy = 32;
    break;
  }

  if (!elantech_support)
    return 0;

  if (elantech_init(kbdc, &elanhw)) {
    printf("psm: couldn't initialize elantech touchpad\n");
    return 0;
  }

  elanhw.dptracex = elanhw.dptracey = 64;
  for (i = 0; i < (int)(sizeof(fw_sizes) / sizeof(fw_sizes[0])); i++) {
    if (elanhw.fwversion == fw_sizes[i][0]) {
      elanhw.sizex = fw_sizes[i][1];
      elanhw.sizey = fw_sizes[i][2];
      goto found;
    }
  }
  if (elantech_cmd(kbdc, hwversion, ELANTECH_FW_ID, resp) != 0) {
    printf("  Failed to read touchpad size\n");
    elanhw.sizex = 10000;
    elanhw.sizey = 10000;
  } else if (hwversion == 2) {
    if ((elanhw.fwversion >> 16) == 0x14 && (resp[1] & 0x10) &&
        !elantech_cmd(kbdc, hwversion, ELANTECH_SAMPLE, resp)) {
      elanhw.dptracex = resp[1] / 2;
      elanhw.dptracey = resp[2] / 2;
    }
    xtr = ((elanhw.fwversion >> 8) == 0x0208) ? 1 : 2;
    elanhw.sizex = (elanhw.ntracesx - xtr) * elanhw.dptracex;
    elanhw.sizey = (elanhw.ntracesy - xtr) * elanhw.dptracey;
  } else {
    elanhw.sizex = (resp[0] & 0x0f) << 8 | resp[1];
    elanhw.sizey = (resp[0] & 0xf0) << 4 | resp[2];
    xtr = (elanhw.sizex % (elanhw.ntracesx - 2) == 0) ? 2 : 1;
    elanhw.dptracex = elanhw.sizex / (elanhw.ntracesx - xtr);
    elanhw.dptracey = elanhw.sizey / (elanhw.ntracesy - xtr);
  }
found:
  if (verbose >= 2) {
    printf("  Model information:\n");
    printf("   MaxX:       %d\n", elanhw.sizex);
    printf("   MaxY:       %d\n", elanhw.sizey);
    printf("   DpmmX:      %d\n", elanhw.dpmmx);
    printf("   DpmmY:      %d\n", elanhw.dpmmy);
    printf("   TracesX:    %d\n", elanhw.ntracesx);
    printf("   TracesY:    %d\n", elanhw.ntracesy);
    printf("   DptraceX:   %d\n", elanhw.dptracex);
    printf("   DptraceY:   %d\n", elanhw.dptracey);
    printf("   SemiMT:     %d\n", elanhw.issemimt);
    printf("   Clickpad:   %d\n", elanhw.isclickpad);
    printf("   Trackpoint: %d\n", elanhw.hastrackpoint);
    printf("   CRC:        %d\n", elanhw.hascrc);
    printf("   Pressure:   %d\n", elanhw.haspressure);
  }

  if (verbose >= 3)
    printf("elantech: END init\n");

  if (arg == PROBE) {
    sc->elanhw = elanhw;
    sc->hw.buttons = 3;
    elantech_init_synaptics(sc);
    for (id = 0; id < ELANTECH_MAX_FINGERS; id++)
      PSM_FINGER_RESET(sc->elanaction.fingers[id]);
  }
  return 1;
}

// SLEEP
// THE DEVICE: psm0 says EWOULDBLOCK, bpsm0 waits
static int psm_cdev_open(struct inode *ip __unused, int omode __unused) {
  struct psm_softc *sc = &psm_sc;
  int error = 0;

  mtx_enter(&kbd_lock);
  if (!(sc->state & PSM_VALID))
    error = FNU_ERR_NODEVICE;
  else if (sc->state & PSM_DEVOPEN)
    error = FNU_ERR_BUSY;
  else {
    sc->state |= PSM_DEVOPEN;
    sc->mode.level = sc->dflt_mode.level;
    sc->mode.protocol = sc->dflt_mode.protocol;
    sc->queue.count = sc->queue.head = sc->queue.tail = 0;
    memset(&sc->status, 0, sizeof(sc->status));
  }
  mtx_leave(&kbd_lock);
  return error;
}

static void psm_cdev_close(struct inode *ip __unused) {
  struct psm_softc *sc = &psm_sc;

  mtx_enter(&kbd_lock);
  sc->state &= ~PSM_DEVOPEN;
  sc->mode.level = sc->dflt_mode.level;
  sc->queue.count = sc->queue.head = sc->queue.tail = 0;
  mtx_leave(&kbd_lock);
}

static int psm_cdev_read(struct inode *ip, struct uio *uio) {
  struct psm_softc *sc = &psm_sc;
  uchar buf[PSM_SMALLBUFSIZE];
  int l, got = 0, error = 0;

  iunlock(ip);
  mtx_enter(&kbd_lock);
  while (sc->queue.count <= 0) {
    if (!(sc->state & PSM_VALID)) {
      error = FNU_ERR_IO;
      break;
    }
    if (!(ip->minor & 1)) {
      error = FNU_ERR_WOULDBLOCK;
      break;
    }
    if (proc_interrupted()) {
      error = FNU_ERR_INTERRUPTED;
      break;
    }
    sc->state |= PSM_ASLP;
    msleep(&sc->queue, &kbd_lock, PZERO | PCATCH, "psmrea", 0);
  }
  while (error == 0 && sc->queue.count > 0 && uio->uio_resid > 0) {
    l = sc->queue.count < (int)uio->uio_resid ? sc->queue.count : (int)uio->uio_resid;
    if (l > (int)sizeof(buf))
      l = sizeof(buf);
    if (l > PSM_BUFSIZE - sc->queue.head) {
      memmove(buf, &sc->queue.buf[sc->queue.head], (size_t)(PSM_BUFSIZE - sc->queue.head));
      memmove(&buf[PSM_BUFSIZE - sc->queue.head], &sc->queue.buf[0],
              (size_t)(l - (PSM_BUFSIZE - sc->queue.head)));
    } else
      memmove(buf, &sc->queue.buf[sc->queue.head], (size_t)l);
    sc->queue.count -= l;
    sc->queue.head = (sc->queue.head + l) % PSM_BUFSIZE;
    mtx_leave(&kbd_lock);
    if (uiomove(buf, (size_t)l, uio) != 0)
      error = FNU_ERR_FAULT;
    else
      got += l;
    mtx_enter(&kbd_lock);
  }
  mtx_leave(&kbd_lock);
  ilock(ip);
  return got > 0 ? got : -error;
}

// level 2 only: each byte a command to the device
static int psm_cdev_write(struct inode *ip, struct uio *uio) {
  struct psm_softc *sc = &psm_sc;
  uchar buf[PSM_SMALLBUFSIZE];
  int i, l, sent = 0, fail = 0;

  if (!(sc->state & PSM_VALID))
    return -FNU_ERR_IO;
  if (sc->mode.level < PSM_LEVEL_NATIVE)
    return -FNU_ERR_WRONGDEVICE;
  iunlock(ip);
  while (uio->uio_resid > 0 && !fail) {
    l = uio->uio_resid < sizeof(buf) ? (int)uio->uio_resid : (int)sizeof(buf);
    if (uiomove(buf, (size_t)l, uio) != 0) {
      ilock(ip);
      return sent > 0 ? sent : -FNU_ERR_FAULT;
    }
    if (!kbdc_lock(sc->kbdc, 1)) {
      ilock(ip);
      return sent > 0 ? sent : -FNU_ERR_BUSY;
    }
    for (i = 0; i < l && !fail; i++) {
      if (verbose >= 4)
        printf("psm0: cmd 0x%x\n", buf[i]);
      fail = !write_aux_command(sc->kbdc, buf[i]);
    }
    kbdc_lock(sc->kbdc, 0);
    sent += i;
  }
  ilock(ip);
  if (fail)
    return reinitialize(sc, 0) != 0 ? -FNU_ERR_IO : sent;
  return sent;
}

static int block_mouse_data(struct psm_softc *sc, int *c) {
  if (!kbdc_lock(sc->kbdc, 1))
    return FNU_ERR_IO;
  *c = get_controller_command_byte(sc->kbdc);
  if (*c == -1 || !set_controller_command_byte(sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                                               KBD_DISABLE_KBD_PORT | KBD_DISABLE_KBD_INT |
                                                   KBD_ENABLE_AUX_PORT | KBD_DISABLE_AUX_INT)) {
    kbdc_lock(sc->kbdc, 0);
    return FNU_ERR_IO;
  }
  empty_aux_buffer(sc->kbdc, 0);
  read_aux_data_no_wait(sc->kbdc);
  mtx_enter(&kbd_lock);
  flushpackets(sc);
  mtx_leave(&kbd_lock);
  return 0;
}

static int unblock_mouse_data(struct psm_softc *sc, int c) {
  int error = 0;

  empty_aux_buffer(sc->kbdc, 0);
  if (!set_controller_command_byte(sc->kbdc, kbdc_get_device_mask(sc->kbdc),
                                   c & (KBD_KBD_CONTROL_BITS | KBD_AUX_CONTROL_BITS)))
    error = FNU_ERR_IO;
  kbdc_lock(sc->kbdc, 0);
  return error;
}

static int res_of(int r) {
  if (r >= 200)
    return FNU_MOUSE_RES_HIGH;
  if (r >= 100)
    return FNU_MOUSE_RES_MEDIUMHIGH;
  if (r >= 50)
    return FNU_MOUSE_RES_MEDIUMLOW;
  if (r > 0)
    return FNU_MOUSE_RES_LOW;
  return r;
}

static int psm_cdev_ioctl(struct inode *ip __unused, uint64_t cmd, uint64_t arg) {
  struct psm_softc *sc = &psm_sc;
  pte_t *pgdir = curproc->p_p->pgdir;
  union {
    struct fnu_mousehw hw;
    struct fnu_mousemode mode;
    struct fnu_mousestatus status;
    struct fnu_mousedata data;
    struct fnu_synapticshw syn;
    int i;
  } u;
  int size = 0, in = 0, error = 0, c, v, stat[3], hw;

  switch (cmd) {
  case FNU_MOUSESETMODE:
    size = sizeof(u.mode);
    in = 1;
    break;
  case FNU_MOUSEREADSTATE:
  case FNU_MOUSEREADDATA:
    size = sizeof(u.data);
    in = 1;
    break;
  case FNU_MOUSESETLEVEL:
  case FNU_MOUSESETRESOLUTION:
  case FNU_MOUSESETSCALING:
  case FNU_MOUSESETRATE:
    size = sizeof(u.i);
    in = 1;
    break;
  }
  if (in && copyin(pgdir, &u, (uintptr_t)arg, (size_t)size) < 0)
    return FNU_ERR_FAULT;

  // the device's own commands own the controller
  hw = cmd == FNU_MOUSESETMODE || cmd == FNU_MOUSEREADSTATE || cmd == FNU_MOUSEREADDATA ||
       cmd == FNU_MOUSESETRESOLUTION || cmd == FNU_MOUSESETRATE ||
       cmd == FNU_MOUSESETSCALING || cmd == FNU_MOUSEGETHWID;
  if (!hw)
    mtx_enter(&kbd_lock);
  switch (cmd) {
  case FNU_MOUSEGETHWINFO:
    u.hw = sc->hw;
    if (sc->mode.level == PSM_LEVEL_BASE)
      u.hw.model = FNU_MOUSE_MODEL_GENERIC;
    size = sizeof(u.hw);
    break;
  case FNU_MOUSESYNGETHWINFO:
    if (sc->synhw.infoMajor >= 4) {
      u.syn = sc->synhw;
      size = sizeof(u.syn);
    } else
      error = FNU_ERR_INVALID;
    break;
  case FNU_MOUSEGETMODE:
    u.mode = sc->mode;
    if (sc->flags & PSM_NEED_SYNCBITS)
      u.mode.syncmask[0] = u.mode.syncmask[1] = 0;
    u.mode.resolution = FNU_MOUSE_RES_LOW - sc->mode.resolution;
    switch (sc->mode.level) {
    case PSM_LEVEL_BASE:
      u.mode.protocol = FNU_MOUSE_PROTO_PS2;
      u.mode.packetsize = FNU_MOUSE_PS2_PACKETSIZE;
      break;
    case PSM_LEVEL_STANDARD:
      u.mode.protocol = FNU_MOUSE_PROTO_SYSMOUSE;
      u.mode.packetsize = FNU_MOUSE_SYS_PACKETSIZE;
      u.mode.syncmask[0] = FNU_MOUSE_SYS_SYNCMASK;
      u.mode.syncmask[1] = FNU_MOUSE_SYS_SYNC;
      break;
    case PSM_LEVEL_NATIVE:
      u.mode.protocol = FNU_MOUSE_PROTO_PS2;
      break;
    }
    size = sizeof(u.mode);
    break;
  case FNU_MOUSESETMODE:
    size = 0;
    if (u.mode.rate > 255 || (u.mode.rate < 0 && u.mode.rate != -1) ||
        u.mode.resolution >= 255 ||
        (u.mode.level != -1 && (u.mode.level < PSM_LEVEL_MIN || u.mode.level > PSM_LEVEL_MAX)) ||
        (u.mode.accelfactor < 0 && u.mode.accelfactor != -1)) {
      error = FNU_ERR_INVALID;
      break;
    }
    if (u.mode.rate == 0)
      u.mode.rate = sc->dflt_mode.rate;
    u.mode.resolution = res_of(u.mode.resolution);
    if (u.mode.resolution == FNU_MOUSE_RES_DEFAULT)
      u.mode.resolution = sc->dflt_mode.resolution;
    else if (u.mode.resolution < -1)
      u.mode.resolution = FNU_MOUSE_RES_LOW - u.mode.resolution;
    if (u.mode.level == -1)
      u.mode.level = sc->mode.level;
    if (u.mode.accelfactor == -1)
      u.mode.accelfactor = sc->mode.accelfactor;
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    if (u.mode.rate > 0)
      u.mode.rate = set_mouse_sampling_rate(sc->kbdc, u.mode.rate);
    if (u.mode.resolution >= 0)
      u.mode.resolution = set_mouse_resolution(sc->kbdc, u.mode.resolution);
    set_mouse_scaling(sc->kbdc, 1);
    get_mouse_status(sc->kbdc, stat, 0, 3);
    mtx_enter(&kbd_lock);
    sc->mode.rate = u.mode.rate;
    sc->mode.resolution = u.mode.resolution;
    sc->mode.accelfactor = u.mode.accelfactor;
    sc->mode.level = u.mode.level;
    mtx_leave(&kbd_lock);
    unblock_mouse_data(sc, c);
    break;
  case FNU_MOUSEGETLEVEL:
    u.i = sc->mode.level;
    size = sizeof(u.i);
    break;
  case FNU_MOUSESETLEVEL:
    size = 0;
    if (u.i < PSM_LEVEL_MIN || u.i > PSM_LEVEL_MAX)
      error = FNU_ERR_INVALID;
    else
      sc->mode.level = u.i;
    break;
  case FNU_MOUSESTATUS:
    u.status = sc->status;
    sc->status.flags = 0;
    sc->status.obutton = sc->status.button;
    sc->status.button = 0;
    sc->status.dx = sc->status.dy = sc->status.dz = 0;
    size = sizeof(u.status);
    break;
  case FNU_MOUSEREADSTATE:
  case FNU_MOUSEREADDATA:
    if (u.data.len > 16 || u.data.len < 0) {
      error = FNU_ERR_INVALID;
      break;
    }
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    if ((u.data.len = get_mouse_status(sc->kbdc, u.data.buf,
                                       cmd == FNU_MOUSEREADDATA, u.data.len)) <= 0)
      error = FNU_ERR_IO;
    unblock_mouse_data(sc, c);
    break;
  case FNU_MOUSESETRESOLUTION:
    size = 0;
    if (u.i >= 255) {
      error = FNU_ERR_INVALID;
      break;
    }
    c = res_of(u.i);
    if (c == FNU_MOUSE_RES_DEFAULT)
      c = sc->dflt_mode.resolution;
    else if (c == -1)
      c = sc->mode.resolution;
    else if (c < 0)
      c = FNU_MOUSE_RES_LOW - c;
    u.i = c;
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    v = set_mouse_resolution(sc->kbdc, u.i);
    mtx_enter(&kbd_lock);
    sc->mode.resolution = v;
    mtx_leave(&kbd_lock);
    if (v != u.i)
      error = FNU_ERR_IO;
    unblock_mouse_data(sc, c);
    break;
  case FNU_MOUSESETRATE:
    size = 0;
    if (u.i > 255) {
      error = FNU_ERR_INVALID;
      break;
    }
    if (u.i == 0)
      u.i = sc->dflt_mode.rate;
    else if (u.i < 0)
      u.i = sc->mode.rate;
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    v = set_mouse_sampling_rate(sc->kbdc, u.i);
    mtx_enter(&kbd_lock);
    sc->mode.rate = v;
    mtx_leave(&kbd_lock);
    if (v != u.i)
      error = FNU_ERR_IO;
    unblock_mouse_data(sc, c);
    break;
  case FNU_MOUSESETSCALING:
    size = 0;
    if (u.i <= 0 || u.i > 2) {
      error = FNU_ERR_INVALID;
      break;
    }
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    if (!set_mouse_scaling(sc->kbdc, u.i))
      error = FNU_ERR_IO;
    unblock_mouse_data(sc, c);
    break;
  case FNU_MOUSEGETHWID:
    if ((error = block_mouse_data(sc, &c)) != 0)
      break;
    v = get_aux_id(sc->kbdc);
    mtx_enter(&kbd_lock);
    sc->hw.hwid = (sc->hw.hwid & ~0x00ff) | v;
    u.i = sc->hw.hwid & 0x00ff;
    mtx_leave(&kbd_lock);
    size = sizeof(u.i);
    unblock_mouse_data(sc, c);
    break;
  case FNU_IONOWAIT:
    size = 0;
    break;
  default:
    error = FNU_ERR_BADREQUEST;
  }
  if (!hw)
    mtx_leave(&kbd_lock);
  if (error == 0 && size > 0 && copyout(pgdir, (uintptr_t)arg, &u, (size_t)size) < 0)
    error = FNU_ERR_FAULT;
  return error;
}

static int psm_cdev_poll(struct inode *ip __unused, int want) {
  struct psm_softc *sc = &psm_sc;
  int ready;

  mtx_enter(&kbd_lock);
  ready = sc->queue.count > 0 || !(sc->state & PSM_VALID);
  if (!ready)
    selrecord(&sc->rsel);
  mtx_leave(&kbd_lock);
  return ready ? want & FNU_SEL_READ : 0;
}

void psm_resume(void) {
  struct psm_softc *sc = &psm_sc;

  if (!(sc->state & PSM_VALID))
    return;
  if (reinitialize(sc, 1) != 0)
    printf("psm0: did not come back from sleep\n");
}

// ATTACH

void psminit(void) {
  struct psm_softc *sc = &psm_sc;

  psm_sysctl_global();
  if (inb(KBD_STATUS_PORT) == 0xff)
    return;
  memset(sc, 0, sizeof(*sc));
  callout_init(&sc->callout);
  callout_init(&sc->softcallout);
  task_set(&sc->recover, psm_recover, sc);
  if ((sc->unit = wsmouse_new()) < 0) {
    printf("psm: no wsmouse unit free\n");
    return;
  }
  if (psmprobe(sc) < 0) {
    wsmouse_release(sc->unit);
    return;
  }
  sc->state = PSM_VALID;
  if (psmopen(sc) < 0) {
    sc->state = 0;
    wsmouse_release(sc->unit);
    printf("psm: cannot open the device\n");
    return;
  }
  if (sc->hw.model == FNU_MOUSE_MODEL_SYNAPTICS || sc->hw.model == FNU_MOUSE_MODEL_ELANTECH) {
    struct fnu_wsmouse_calib c = {
        .minx = sc->synhw.minimumXCoord, .maxx = sc->synhw.maximumXCoord,
        .miny = sc->synhw.minimumYCoord, .maxy = sc->synhw.maximumYCoord,
        .resx = sc->synhw.infoXupmm, .resy = sc->synhw.infoYupmm};

    wsmouse_set_hw(sc->unit, sc->hw.model == FNU_MOUSE_MODEL_SYNAPTICS
                                 ? FNU_WSMOUSE_TYPE_SYNAPTICS
                                 : FNU_WSMOUSE_TYPE_ELANTECH,
                   &c);
  } else
    wsmouse_set_hw(sc->unit, FNU_WSMOUSE_TYPE_PS2, 0);
  if (sc->syninfo_set)
    synaptics_sysctl_create_tree(sc);
  if (sc->tptree)
    trackpoint_sysctl_create_tree(sc);
  wsmouse_attach(sc->unit);
  hotplug_attach(FNU_DEVCLASS_MOUSE, wsmouse_name(sc->unit), 0, 0);
  devsw[PSM].class = FNU_DEVCLASS_MOUSE;
  devsw[PSM].open = psm_cdev_open;
  devsw[PSM].close = psm_cdev_close;
  devsw[PSM].read = psm_cdev_read;
  devsw[PSM].write = psm_cdev_write;
  devsw[PSM].ioctl = psm_cdev_ioctl;
  devsw[PSM].poll = psm_cdev_poll;
  devfs_add("psm0", FNU_TYPE_DEVICE, PSM, 0, 0600);
  devfs_add("bpsm0", FNU_TYPE_DEVICE, PSM, 1, 0600);
  printf("psm0: %s, model %s, device ID %d, %d buttons, %s\n",
          sc->hw.type == FNU_MOUSE_TRACKBALL ? "trackball" : "PS/2 mouse",
          model_name(sc->hw.model), sc->hw.hwid & 0x00ff, sc->hw.buttons,
          wsmouse_name(sc->unit));
  if (verbose >= 2)
    printf("psm0: config:%x, flags:%x, packet size:%d, syncmask:%x, syncbits:%x\n",
            sc->config, sc->flags, sc->mode.packetsize, sc->mode.syncmask[0],
            sc->mode.syncmask[1]);
}
