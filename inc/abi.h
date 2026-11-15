#ifndef PRONINX_X86_64_ABI_H
#define PRONINX_X86_64_ABI_H

#include "inc/ioccom.h"
#include "inc/types.h"

// SYS_info
struct fnu_info {
  uint64 uptime;
  uint64 total_ram;
  uint64 free_ram;
  uint32 nprocs;
  uint32 pad;
  uint64 swap_total;
  uint64 swap_used;
  uint64 swap_outs;
  uint64 swap_ins;
  uint32 loadavg[3];
  uint32 pad2;
};

#define FNU_FSCALE 2048

#define FNU_EXIT_NORMAL 1
#define FNU_EXIT_KILLED 2
#define FNU_EXIT_STOPPED 3
#define FNU_EXIT_CONTINUED 4

struct fnu_usage {
  struct fnu_timespec utime;
  struct fnu_timespec stime;
  int64_t maxrss; // KB
  int64_t nsignals;
  int64_t nvcsw;
  int64_t nivcsw;
};

struct fnu_exitinfo {
  pid_t pid;
  int32_t reason;
  int32_t status;
  uint32_t uid;
  struct fnu_usage ru;
};

struct fnu_procids {
  pid_t pid;
  pid_t ppid;
  pid_t pgid;
  pid_t sid;
};

#define FNU_GROUPS_MAX 16
#define FNU_LOGIN_MAX 33

struct fnu_ident {
  uint32 uid;
  uint32 euid;
  uint32 svuid; // saved
  uint32 gid;
  uint32 egid;
  uint32 svgid;
  uint32 ngroups;
  uint32 groups[FNU_GROUPS_MAX];
  uint32 changed;
  char login[FNU_LOGIN_MAX];
  char pad[7];
};

#define FNU_IDENT_UID 0x1
#define FNU_IDENT_GID 0x2
#define FNU_IDENT_GROUPS 0x4
#define FNU_IDENT_LOGIN 0x8
#define FNU_IDENT_RUID 0x10
#define FNU_IDENT_EUID 0x20
#define FNU_IDENT_SVUID 0x40
#define FNU_IDENT_RGID 0x80
#define FNU_IDENT_EGID 0x100
#define FNU_IDENT_SVGID 0x200

#define FNU_WAIT_NOHANG 1
#define FNU_WAIT_STOPPED 2
#define FNU_WAIT_CONTINUED 4

// SYS_procinfo
#define FNU_PS_UNUSED 0
#define FNU_PS_EMBRYO 1
#define FNU_PS_SLEEPING 2
#define FNU_PS_RUNNABLE 3
#define FNU_PS_RUNNING 4
#define FNU_PS_ZOMBIE 5
#define FNU_PS_STOPPED 6
#define FNU_PS_DEAD 7

#define FNU_PROCARGS_MAX 128
#define FNU_PROC_LEADER 0x01
#define FNU_PROC_CTTY 0x02
#define FNU_PROC_FOREGROUND 0x04
#define FNU_PROC_TRACED 0x08
#define FNU_PROC_SUGID 0x10
#define FNU_PROC_PLEDGED 0x20
#define FNU_PROC_SYSTEM 0x40

struct fnu_procinfo {
  pointer_t pid;
  proc_state_t state;
  char name[16];
  pointer_t memory_size;
  uint32 priority; // smaller is sooner
  int32 nice;
  uint32 pctcpu; // FNU_FSCALE parts
  uint32 flags;
  int32 ppid;
  int32 pgid;
  int32 sid;
  int32 tpgid; // 0 without one
  uint32 uid;
  uint32 ruid;
  uint32 gid;
  uint32 rgid;
  uint32 nthreads;
  uint32 pad;
  int64 start; // uptime ns
  int64 utime; // us
  int64 stime;
  uint64 rss;  // KB
  int64 ttyinput; // uptime ns, 0 never
  char tty[16];
  char wchan[16];
  char login[FNU_LOGIN_MAX];
  char pad2[7];
  char args[FNU_PROCARGS_MAX];
};

// THE TERMINAL'S MODE
#define FNU_TTYKEY_EOF 0
#define FNU_TTYKEY_EOL 1
#define FNU_TTYKEY_EOL2 2
#define FNU_TTYKEY_ERASE 3
#define FNU_TTYKEY_WORDERASE 4
#define FNU_TTYKEY_KILL 5
#define FNU_TTYKEY_REPRINT 6
#define FNU_TTYKEY_ERASE2 7
#define FNU_TTYKEY_INTERRUPT 8
#define FNU_TTYKEY_QUIT 9
#define FNU_TTYKEY_SUSPEND 10
#define FNU_TTYKEY_START 11
#define FNU_TTYKEY_STOP 12
#define FNU_TTYKEY_LITERAL 13
#define FNU_TTYKEY_DISCARD 14
#define FNU_TTYKEY_STATUS 15
#define FNU_TTYKEY_DELAYSUSP 16
#define FNU_TTYKEY_COUNT 20
#define FNU_TTYKEY_OFF 0xff

struct fnu_ttymode {
  uint32 in;
  uint32 out;
  uint32 line;
  uint32 edit;
  uint8 key[FNU_TTYKEY_COUNT];
  uint8 min;
  uint8 time; // tenths
  uint16 pad;
  uint32 speed_in;
  uint32 speed_out;
};

#define FNU_TTY_SPEED 9600

#define FNU_TTYDEF_IN (FNU_TTYIN_CRTONL | FNU_TTYIN_FLOWOUT | FNU_TTYIN_BELLFULL | \
                       FNU_TTYIN_BREAKINT)
#define FNU_TTYDEF_OUT (FNU_TTYOUT_PROCESS | FNU_TTYOUT_NLTOCRNL)
#define FNU_TTYDEF_LINE (FNU_TTYLINE_8BITS | FNU_TTYLINE_RECEIVE | FNU_TTYLINE_LOCAL)
#define FNU_TTYDEF_EDIT (FNU_TTYEDIT_SIGNALS | FNU_TTYEDIT_LINES | FNU_TTYEDIT_ECHO | \
                         FNU_TTYEDIT_ECHOERASE | FNU_TTYEDIT_ECHOKILLRUB | \
                         FNU_TTYEDIT_ECHOCTL | FNU_TTYEDIT_EXTENDED)
#define FNU_TTYDEF_KEYS                                                       \
  {004,  0xff, 0xff, 0177, 027,  025,  022,  010,  003,  034,                 \
   032,  021,  023,  026,  017,  024,  031,  0xff, 0xff, 0xff}

// what comes in
#define FNU_TTYIN_STRIP 0x01
#define FNU_TTYIN_NLTOCR 0x02
#define FNU_TTYIN_DROPCR 0x04
#define FNU_TTYIN_CRTONL 0x08
#define FNU_TTYIN_FLOWOUT 0x10
#define FNU_TTYIN_FLOWIN 0x20
#define FNU_TTYIN_ANYSTART 0x40
#define FNU_TTYIN_BELLFULL 0x80
#define FNU_TTYIN_IGNBREAK 0x100
#define FNU_TTYIN_BREAKINT 0x200
#define FNU_TTYIN_IGNPARITY 0x400
#define FNU_TTYIN_MARKPARITY 0x800
#define FNU_TTYIN_CHECKPARITY 0x1000
#define FNU_TTYIN_TOLOWER 0x2000

// what goes out
#define FNU_TTYOUT_PROCESS 0x1
#define FNU_TTYOUT_NLTOCRNL 0x2
#define FNU_TTYOUT_CRTONL 0x4
#define FNU_TTYOUT_NLRETURNS 0x8
#define FNU_TTYOUT_TABS 0x10
#define FNU_TTYOUT_NOEOT 0x20
#define FNU_TTYOUT_TOUPPER 0x40
#define FNU_TTYOUT_NOCRCOL0 0x80

// the wire
#define FNU_TTYLINE_BITS 0x3
#define FNU_TTYLINE_5BITS 0x0
#define FNU_TTYLINE_6BITS 0x1
#define FNU_TTYLINE_7BITS 0x2
#define FNU_TTYLINE_8BITS 0x3
#define FNU_TTYLINE_TWOSTOP 0x4
#define FNU_TTYLINE_RECEIVE 0x8
#define FNU_TTYLINE_PARITY 0x10
#define FNU_TTYLINE_ODD 0x20
#define FNU_TTYLINE_HANGUP 0x40
#define FNU_TTYLINE_LOCAL 0x80
#define FNU_TTYLINE_CTSFLOW 0x100
#define FNU_TTYLINE_RTSFLOW 0x200
#define FNU_TTYLINE_HWFLOW (FNU_TTYLINE_CTSFLOW | FNU_TTYLINE_RTSFLOW)
#define FNU_TTYLINE_DSRFLOW 0x400
#define FNU_TTYLINE_CARRIERFLOW 0x800
#define FNU_TTYLINE_IGNORE 0x1000

// the line discipline
#define FNU_TTYEDIT_ECHO 0x001
#define FNU_TTYEDIT_ECHOERASE 0x002
#define FNU_TTYEDIT_ECHOKILL 0x004
#define FNU_TTYEDIT_ECHOKILLRUB 0x008
#define FNU_TTYEDIT_ECHONL 0x010
#define FNU_TTYEDIT_ECHOCTL 0x020
#define FNU_TTYEDIT_SIGNALS 0x040
#define FNU_TTYEDIT_LINES 0x080
#define FNU_TTYEDIT_EXTENDED 0x100
#define FNU_TTYEDIT_STOPBG 0x200
#define FNU_TTYEDIT_NOFLUSH 0x400
#define FNU_TTYEDIT_ECHOPRINT 0x800
#define FNU_TTYEDIT_ALTWERASE 0x1000
#define FNU_TTYEDIT_FLUSHOUT 0x2000
#define FNU_TTYEDIT_PENDING 0x4000
#define FNU_TTYEDIT_NOSTATUS 0x8000
#define FNU_TTYEDIT_EXTERNAL 0x10000
#define FNU_TTYEDIT_CASEESC 0x20000

struct fnu_ttysize {
  uint16 rows;
  uint16 cols;
  uint16 xpixels;
  uint16 ypixels;
};

#define FNU_TTYGETMODE _IOR('T', 1, struct fnu_ttymode)
#define FNU_TTYSETMODE _IOW('T', 2, struct fnu_ttymode)
#define FNU_TTYSETMODE_DRAIN _IOW('T', 3, struct fnu_ttymode)
#define FNU_TTYSETMODE_FLUSH _IOW('T', 4, struct fnu_ttymode)
#define FNU_TTYGETSIZE _IOR('T', 5, struct fnu_ttysize)
#define FNU_TTYSETSIZE _IOW('T', 6, struct fnu_ttysize)
#define FNU_TTYSTART _IO('T', 7)
#define FNU_TTYSTOP _IO('T', 8)
#define FNU_TTYSTATUS _IO('T', 9)
#define FNU_TTYEXTERNAL _IOW('T', 10, int)

// ptm(4)
struct fnu_ptmget {
  int32 cfd;
  int32 sfd;
  char cn[16];
  char sn[16];
};
#define FNU_PTMGET _IOR('t', 1, struct fnu_ptmget)

#define FNU_FLUSH_INPUT 0x1
#define FNU_FLUSH_OUTPUT 0x2
#define FNU_TTYFLUSH _IOW('t', 16, int)
#define FNU_TTYCONTROL _IO('t', 97)
#define FNU_TTYGETSESSION _IOR('t', 99, int)
#define FNU_TTYSETGROUP _IOW('t', 118, int)
#define FNU_TTYGETGROUP _IOR('t', 119, int)
// sys/filio.h
#define FNU_IOCLOSEEXEC _IO('f', 1)
#define FNU_IOKEEPEXEC _IO('f', 2)
#define FNU_IOWAITING _IOR('f', 127, int)
#define FNU_IONOWAIT _IOW('f', 126, int)

#define FNU_PIPE_ATOMIC 512

#define FNU_TTYDRAIN _IO('t', 94)
#define FNU_TTYGETLINES _IOR('t', 106, int)
#define FNU_TTYDROPLINES _IOW('t', 107, int)
#define FNU_TTYRAISELINES _IOW('t', 108, int)
#define FNU_TTYSETLINES _IOW('t', 109, int)
#define FNU_TTYOUTQ _IOR('t', 115, int)
#define FNU_TTYDROPDTR _IO('t', 120)
#define FNU_TTYRAISEDTR _IO('t', 121)
#define FNU_TTYENDBREAK _IO('t', 122)
#define FNU_TTYBREAK _IO('t', 123)

#define FNU_TTYMODEM_LE 0001
#define FNU_TTYMODEM_DTR 0002
#define FNU_TTYMODEM_RTS 0004
#define FNU_TTYMODEM_ST 0010
#define FNU_TTYMODEM_SR 0020
#define FNU_TTYMODEM_CTS 0040
#define FNU_TTYMODEM_CD 0100
#define FNU_TTYMODEM_RI 0200
#define FNU_TTYMODEM_DSR 0400

// sys/disk.h
#define FNU_DISKSECTOR _IOR('d', 128, uint32)
#define FNU_DISKSIZE _IOR('d', 129, int64)

// sys/fbio.h
#define FNU_FBTYPE_PCIMISC 11
#define FNU_FBTYPE_VGA 24
#define FNU_FBTYPE_EFIFB 31

struct fnu_fbtype {
    int fb_type;
    int fb_height;
    int fb_width;
    int fb_depth;
    int fb_cmsize;
    int fb_size;
};
#define FNU_FBTYPE _IOR('F', 0, struct fnu_fbtype)

struct fnu_video_info {
    int vi_mode;
    int vi_flags;
#define FNU_V_INFO_COLOR (1 << 0)
#define FNU_V_INFO_GRAPHICS (1 << 1)
#define FNU_V_INFO_LINEAR (1 << 2)
#define FNU_V_INFO_VESA (1 << 3)
#define FNU_V_INFO_NONVGA (1 << 4)
    int vi_width;
    int vi_height;
    int vi_cwidth;
    int vi_cheight;
    int vi_depth;
    int vi_planes;
    uint64 vi_window;
    uint64 vi_window_size;
    uint64 vi_window_gran;
    uint64 vi_buffer;
    uint64 vi_buffer_size;
    int vi_mem_model;
#define FNU_V_INFO_MM_OTHER (-1)
#define FNU_V_INFO_MM_TEXT 0
#define FNU_V_INFO_MM_PLANAR 1
#define FNU_V_INFO_MM_PACKED 2
#define FNU_V_INFO_MM_DIRECT 3
    int vi_pixel_size;
    int vi_pixel_fields[4];
    int vi_pixel_fsizes[4];
    uint8 vi_reserved[64];
    uint64 vi_registers;
    uint64 vi_registers_size;
};

#define FNU_M_VESA_BASE 0x100
#define FNU_M_VESA_MODE_MAX 0x1ff

#define FNU_FBMODEINFO _IOWR('F', 103, struct fnu_video_info)
#define FNU_FBFINDMODE _IOWR('F', 104, struct fnu_video_info)
#define FNU_FBGETMODE _IOR('F', 105, int)
#define FNU_FBSETMODE _IOW('F', 106, int)

// consio.h and mouse.h

struct fnu_mouse_data {
    int x;
    int y;
    int z;
    int buttons;
};

struct fnu_mouse_event {
    int id;
    int value;
};

struct fnu_mouse_info {
    int operation;
#define FNU_MOUSE_SHOW 0x01
#define FNU_MOUSE_HIDE 0x02
#define FNU_MOUSE_MOVEABS 0x03
#define FNU_MOUSE_MOVEREL 0x04
#define FNU_MOUSE_GETINFO 0x05
#define FNU_MOUSE_ACTION 0x07
#define FNU_MOUSE_MOTION_EVENT 0x08
#define FNU_MOUSE_BUTTON_EVENT 0x09
    union {
        struct fnu_mouse_data data;
        struct fnu_mouse_event event;
    } u;
};
#define FNU_CONSMOUSE _IOWR('c', 10, struct fnu_mouse_info)

#define FNU_MOUSE_BUTTON1DOWN 0x0001
#define FNU_MOUSE_BUTTON2DOWN 0x0002
#define FNU_MOUSE_BUTTON3DOWN 0x0004
#define FNU_MOUSE_BUTTON4DOWN 0x0008
#define FNU_MOUSE_BUTTON5DOWN 0x0010
#define FNU_MOUSE_BUTTON6DOWN 0x0020
#define FNU_MOUSE_BUTTON7DOWN 0x0040
#define FNU_MOUSE_BUTTON8DOWN 0x0080
#define FNU_MOUSE_MAXBUTTON 31
#define FNU_MOUSE_STDBUTTONS 0x0007
#define FNU_MOUSE_BUTTONS 0x7fffffff
#define FNU_MOUSE_EXTBUTTONS 0x7ffffff8
#define FNU_MOUSE_POSCHANGED 0x80000000

struct fnu_mousestatus {
    int flags;
    int button;
    int obutton;
    int dx;
    int dy;
    int dz;
};

#define FNU_MOUSESTATUS _IOR('M', 0, struct fnu_mousestatus)
#define FNU_MOUSEGETLEVEL _IOR('M', 4, int)
#define FNU_MOUSESETLEVEL _IOW('M', 5, int)

#define FNU_MOUSE_MSC_PACKETSIZE 5
#define FNU_MOUSE_MSC_SYNCMASK 0xf8
#define FNU_MOUSE_MSC_SYNC 0x80
#define FNU_MOUSE_MSC_BUTTONS 0x07
#define FNU_MOUSE_MSC_BUTTON1UP 0x04
#define FNU_MOUSE_MSC_BUTTON2UP 0x02
#define FNU_MOUSE_MSC_BUTTON3UP 0x01
#define FNU_MOUSE_SYS_PACKETSIZE 8
#define FNU_MOUSE_SYS_SYNCMASK 0xf8
#define FNU_MOUSE_SYS_SYNC 0x80

struct fnu_mousehw {
    int buttons;
    int iftype;
    int type;
    int model;
    int hwid;
};
#define FNU_MOUSE_IF_UNKNOWN (-1)
#define FNU_MOUSE_IF_SERIAL 0
#define FNU_MOUSE_IF_PS2 3
#define FNU_MOUSE_IF_SYSMOUSE 4
#define FNU_MOUSE_IF_USB 5
#define FNU_MOUSE_UNKNOWN (-1)
#define FNU_MOUSE_MOUSE 0
#define FNU_MOUSE_TRACKBALL 1
#define FNU_MOUSE_STICK 2
#define FNU_MOUSE_PAD 3
#define FNU_MOUSE_MODEL_UNKNOWN (-1)
#define FNU_MOUSE_MODEL_GENERIC 0
#define FNU_MOUSE_MODEL_GLIDEPOINT 1
#define FNU_MOUSE_MODEL_NETSCROLL 2
#define FNU_MOUSE_MODEL_NET 3
#define FNU_MOUSE_MODEL_INTELLI 4
#define FNU_MOUSE_MODEL_THINK 5
#define FNU_MOUSE_MODEL_EASYSCROLL 6
#define FNU_MOUSE_MODEL_MOUSEMANPLUS 7
#define FNU_MOUSE_MODEL_KIDSPAD 8
#define FNU_MOUSE_MODEL_VERSAPAD 9
#define FNU_MOUSE_MODEL_EXPLORER 10
#define FNU_MOUSE_MODEL_4D 11
#define FNU_MOUSE_MODEL_4DPLUS 12
#define FNU_MOUSE_MODEL_SYNAPTICS 13
#define FNU_MOUSE_MODEL_TRACKPOINT 14
#define FNU_MOUSE_MODEL_ELANTECH 15

struct fnu_mousemode {
    int protocol;
    int rate;
    int resolution;
    int accelfactor;
    int level;
    int packetsize;
    uchar syncmask[2];
};
#define FNU_MOUSE_PROTO_UNKNOWN (-1)
#define FNU_MOUSE_PROTO_PS2 7
#define FNU_MOUSE_PROTO_SYSMOUSE 12
#define FNU_MOUSE_RES_UNKNOWN (-1)
#define FNU_MOUSE_RES_DEFAULT 0
#define FNU_MOUSE_RES_LOW (-2)
#define FNU_MOUSE_RES_MEDIUMLOW (-3)
#define FNU_MOUSE_RES_MEDIUMHIGH (-4)
#define FNU_MOUSE_RES_HIGH (-5)
#define FNU_MOUSE_LEVEL_BASE 0
#define FNU_MOUSE_LEVEL_STANDARD 1
#define FNU_MOUSE_LEVEL_NATIVE 2

struct fnu_mousedata {
    int len;
    int buf[16];
};

struct fnu_synapticshw {
    int infoMajor, infoMinor, infoRot180, infoPortrait, infoSensor, infoHardware;
    int infoNewAbs, capPen, infoSimplC, infoGeometry, capExtended, capSleep;
    int capFourButtons, capMultiFinger, capPalmDetect, capPassthrough, capMiddle;
    int capLowPower, capMultiFingerReport, capBallistics, nExtendedButtons;
    int nExtendedQueries, capClickPad, capDeluxeLEDs, noAbsoluteFilter;
    int capReportsV, capUniformClickPad, capReportsMin, capInterTouch;
    int capReportsMax, capClearPad, capAdvancedGestures, multiFingerMode;
    int capCoveredPad, verticalScroll, horizontalScroll, verticalWheel;
    int capEWmode, minimumXCoord, minimumYCoord, maximumXCoord, maximumYCoord;
    int infoXupmm, infoYupmm, forcePad, topButtonPad;
};

#define FNU_MOUSEGETHWINFO _IOR('M', 1, struct fnu_mousehw)
#define FNU_MOUSEGETMODE _IOR('M', 2, struct fnu_mousemode)
#define FNU_MOUSESETMODE _IOW('M', 3, struct fnu_mousemode)
#define FNU_MOUSEREADSTATE _IOWR('M', 8, struct fnu_mousedata)
#define FNU_MOUSEREADDATA _IOWR('M', 9, struct fnu_mousedata)
#define FNU_MOUSESETRESOLUTION _IOW('M', 10, int)
#define FNU_MOUSESETSCALING _IOW('M', 11, int)
#define FNU_MOUSESETRATE _IOW('M', 12, int)
#define FNU_MOUSEGETHWID _IOR('M', 13, int)
#define FNU_MOUSESYNGETHWINFO _IOR('M', 100, struct fnu_synapticshw)

// level 0
#define FNU_MOUSE_PS2_PACKETSIZE 3
#define FNU_MOUSE_PS2_SYNCMASK 0xc8
#define FNU_MOUSE_PS2_SYNC 0x08
#define FNU_MOUSE_PS2_BUTTONS 0x07
#define FNU_MOUSE_PS2_BUTTON1DOWN 0x01
#define FNU_MOUSE_PS2_BUTTON2DOWN 0x04
#define FNU_MOUSE_PS2_BUTTON3DOWN 0x02
#define FNU_MOUSE_PS2_TAP FNU_MOUSE_PS2_SYNC
#define FNU_MOUSE_PS2_XNEG 0x10
#define FNU_MOUSE_PS2_YNEG 0x20
#define FNU_MOUSE_PS2_XOVERFLOW 0x40
#define FNU_MOUSE_PS2_YOVERFLOW 0x80

// THE KEYBOARD
#define FNU_KB_RAW 0
#define FNU_KB_XLATE 1
#define FNU_KB_CODE 2
#define FNU_KBGETMODE _IOR('K', 6, int)
#define FNU_KBSETMODE _IOWINT('K', 7)

#define FNU_KB_CLKED 1
#define FNU_KB_NLKED 2
#define FNU_KB_SLKED 4
#define FNU_KB_ALKED 8
#define FNU_KB_LOCKS (FNU_KB_CLKED | FNU_KB_NLKED | FNU_KB_SLKED | FNU_KB_ALKED)
#define FNU_KBGETLOCKS _IOR('K', 19, int)
#define FNU_KBSETLOCKS _IOWINT('K', 20)

#define FNU_KB_OTHER 0
#define FNU_KB_84 1
#define FNU_KB_101 2
#define FNU_KBGETTYPE _IOR('K', 64, int)

#define FNU_KB_LEDCAP 1
#define FNU_KB_LEDNUM 2
#define FNU_KB_LEDSCR 4
#define FNU_KB_LEDS (FNU_KB_LEDCAP | FNU_KB_LEDNUM | FNU_KB_LEDSCR)
#define FNU_KBGETLEDS _IOR('K', 65, int)
#define FNU_KBSETLEDS _IOWINT('K', 66)
#define FNU_KBSETRATE _IOWINT('K', 67) // the i8042 typematic byte

struct fnu_kbinfo {
    int kb_index;
    char kb_name[16];
    int kb_unit;
    int kb_type;
    int kb_config;
    int kb_flags;
};
#define FNU_KBINFO _IOR('K', 101, struct fnu_kbinfo)

struct fnu_kbrepeat {
    int kb_repeat[2]; // delay, period; ms
};
#define FNU_KBSETREPEAT _IOW('K', 102, struct fnu_kbrepeat)
#define FNU_KBGETREPEAT _IOR('K', 103, struct fnu_kbrepeat)

#define FNU_KB_NKEYS 256
#define FNU_KB_NSTATES 8
#define FNU_KB_ALTGR 128
#define FNU_KB_NACCENTS 15
#define FNU_KB_NACCENTCHARS 52
#define FNU_KB_NFKEYS 96
#define FNU_KB_FKEYLEN 16

struct fnu_keyent {
    uint32 map[FNU_KB_NSTATES];
    uchar spcl;
    uchar flgs;
#define FNU_KEY_LOCK_O 0
#define FNU_KEY_LOCK_C 1
#define FNU_KEY_LOCK_N 2
};

struct fnu_keymap {
    uint16 n_keys;
    struct fnu_keyent key[FNU_KB_NKEYS];
};

// actions
#define FNU_KEY_NOP 0x00
#define FNU_KEY_LSH 0x02
#define FNU_KEY_RSH 0x03
#define FNU_KEY_CLK 0x04
#define FNU_KEY_NLK 0x05
#define FNU_KEY_SLK 0x06
#define FNU_KEY_LALT 0x07
#define FNU_KEY_BTAB 0x08
#define FNU_KEY_LCTR 0x09
#define FNU_KEY_NEXT 0x0a
#define FNU_KEY_F_SCR 0x0b
#define FNU_KEY_L_SCR 0x1a
#define FNU_KEY_F_FN 0x1b
#define FNU_KEY_L_FN 0x7a
#define FNU_KEY_RCTR 0x80
#define FNU_KEY_RALT 0x81
#define FNU_KEY_ALK 0x82
#define FNU_KEY_ASH 0x83
#define FNU_KEY_META 0x84
#define FNU_KEY_RBT 0x85
#define FNU_KEY_DBG 0x86
#define FNU_KEY_SUSP 0x87
#define FNU_KEY_SPSC 0x88
#define FNU_KEY_F_ACC FNU_KEY_DGRA
#define FNU_KEY_DGRA 0x89
#define FNU_KEY_DACU 0x8a
#define FNU_KEY_DCIR 0x8b
#define FNU_KEY_DTIL 0x8c
#define FNU_KEY_DMAC 0x8d
#define FNU_KEY_DBRE 0x8e
#define FNU_KEY_DDOT 0x8f
#define FNU_KEY_DUML 0x90
#define FNU_KEY_DDIA 0x90
#define FNU_KEY_DSLA 0x91
#define FNU_KEY_DRIN 0x92
#define FNU_KEY_DCED 0x93
#define FNU_KEY_DAPO 0x94
#define FNU_KEY_DDAC 0x95
#define FNU_KEY_DOGO 0x96
#define FNU_KEY_DCAR 0x97
#define FNU_KEY_L_ACC FNU_KEY_DCAR
#define FNU_KEY_STBY 0x98
#define FNU_KEY_PREV 0x99
#define FNU_KEY_PNC 0x9a
#define FNU_KEY_LSHA 0x9b
#define FNU_KEY_RSHA 0x9c
#define FNU_KEY_LCTRA 0x9d
#define FNU_KEY_RCTRA 0x9e
#define FNU_KEY_LALTA 0x9f
#define FNU_KEY_RALTA 0xa0
#define FNU_KEY_HALT 0xa1
#define FNU_KEY_PDWN 0xa2
#define FNU_KEY_PASTE 0xa3

#define FNU_KEY_F(x) ((x) + FNU_KEY_F_FN - 1)
#define FNU_KEY_S(x) ((x) + FNU_KEY_F_SCR - 1)
#define FNU_KEY_ACC(x) ((x) + FNU_KEY_F_ACC)

struct fnu_accent {
    uint32 accchar;
    uint32 map[FNU_KB_NACCENTCHARS][2];
};

struct fnu_accentmap {
    uint16 n_accs;
    struct fnu_accent acc[FNU_KB_NACCENTS];
};

struct fnu_keyarg {
    uint16 keynum;
    struct fnu_keyent key;
};

struct fnu_fkeytab {
    uchar str[FNU_KB_FKEYLEN];
    uchar len;
};

struct fnu_fkeyarg {
    uint16 keynum;
    char keydef[FNU_KB_FKEYLEN];
    uchar flen;
};

#define FNU_KBGETFKEY _IOWR('k', 0, struct fnu_fkeyarg)
#define FNU_KBSETFKEY _IOWR('k', 1, struct fnu_fkeyarg)
#define FNU_KBGETMAP _IO('k', 6)
#define FNU_KBSETMAP _IO('k', 7)
#define FNU_KBGETACCENTS _IO('k', 8)
#define FNU_KBSETACCENTS _IO('k', 9)
#define FNU_KBGETKEY _IOWR('k', 10, struct fnu_keyarg)
#define FNU_KBSETKEY _IOW('k', 11, struct fnu_keyarg)

#define FNU_NOKEY 0x01000000
#define FNU_FKEY 0x02000000
#define FNU_MKEY 0x04000000
#define FNU_BKEY 0x08000000
#define FNU_SPCLKEY 0x80000000
#define FNU_RELKEY 0x40000000
#define FNU_ERRKEY 0x20000000

#define FNU_KEYCHAR(c) ((c) & 0x00ffffff)
#define FNU_KEYFLAGS(c) ((c) & ~0x00ffffff)

struct fnu_network_endpoint { uint8 address[4]; uint16 port; };

struct fnu_ifconfig {
  char name[16];
  uint8 address[4];
  uint8 netmask[4];
  uint8 gateway[4];
  uint8 dns_server[4];
  uint32 dhcp;
};

// SYS_netinfo
#define FNU_LINK_UNKNOWN 0
#define FNU_LINK_DOWN 2
#define FNU_LINK_UP 4
#define FNU_LINK_FULL_DUPLEX 6
#define FNU_IFF_DHCP 0x1
#define FNU_IFF_DEFAULT 0x2
struct fnu_ifinfo {
  char name[16];
  uint32 index;
  uint32 mtu;
  uint32 flags;
  uint8 lladdr[6];
  uint8 link;
  uint8 _pad0[5];
  uint64 baudrate;
  uint8 address[4];
  uint8 netmask[4];
  uint8 gateway[4];
  uint8 _pad1[4];
  uint64 ipackets, ibytes, iqdrops;
  uint64 opackets, obytes, oerrors;
};

// mmap(2)
#define FNU_MEM_NONE 0x0
#define FNU_MEM_READ 0x1
#define FNU_MEM_WRITE 0x2
#define FNU_MEM_EXEC 0x4

#define FNU_MEM_SHARED 0x0001
#define FNU_MEM_PRIVATE 0x0002
#define FNU_MEM_FIXED 0x0010
#define FNU_MEM_NOREPLACE 0x0800
#define FNU_MEM_ANON 0x1000
// sysctl
#define FNU_CTL_INT 1
#define FNU_CTL_QUAD 2
#define FNU_CTL_STRING 3
#define FNU_CTL_OPAQUE 4
#define FNU_CTLF_WRITE 0x01
#define FNU_CTLF_STDIO 0x02
#define FNU_CTLF_PRIVATE 0x04
#define FNU_CTL_MALLOC_MAX 64

#define FNU_CTL_NAMELEN 64

struct fnu_sysctl_info {
  char name[FNU_CTL_NAMELEN];
  uint32 type;
  uint32 flags;
};

#define FNU_PLEDGE_ALWAYS 0xffffffffffffffffULL
#define FNU_PLEDGE_RPATH 0x0000000000000001ULL
#define FNU_PLEDGE_WPATH 0x0000000000000002ULL
#define FNU_PLEDGE_CPATH 0x0000000000000004ULL
#define FNU_PLEDGE_STDIO 0x0000000000000008ULL
#define FNU_PLEDGE_DNS 0x0000000000000020ULL
#define FNU_PLEDGE_INET 0x0000000000000040ULL
// fnu_flock
#define FNU_LOCK_SH 1
#define FNU_LOCK_EX 2
#define FNU_LOCK_NB 4
#define FNU_LOCK_UN 8

#define FNU_PLEDGE_FLOCK 0x0000000000000080ULL
#define FNU_PLEDGE_UNIX 0x0000000000000100ULL
#define FNU_PLEDGE_ID 0x0000000000000200ULL
#define FNU_PLEDGE_TAPE 0x0000000000000400ULL
#define FNU_PLEDGE_GETPW 0x0000000000000800ULL
#define FNU_PLEDGE_PROC 0x0000000000001000ULL
#define FNU_PLEDGE_SETTIME 0x0000000000002000ULL
#define FNU_PLEDGE_FATTR 0x0000000000004000ULL
#define FNU_PLEDGE_PROTEXEC 0x0000000000008000ULL
#define FNU_PLEDGE_TTY 0x0000000000010000ULL
#define FNU_PLEDGE_SENDFD 0x0000000000020000ULL
#define FNU_PLEDGE_RECVFD 0x0000000000040000ULL
#define FNU_PLEDGE_EXEC 0x0000000000080000ULL
#define FNU_PLEDGE_ROUTE 0x0000000000100000ULL
#define FNU_PLEDGE_VMINFO 0x0000000000400000ULL
#define FNU_PLEDGE_PS 0x0000000000800000ULL
#define FNU_PLEDGE_DISKLABEL 0x0000000002000000ULL
#define FNU_PLEDGE_AUDIO 0x0000000008000000ULL
#define FNU_PLEDGE_DPATH 0x0000000010000000ULL
#define FNU_PLEDGE_CHOWN 0x0000000040000000ULL
#define FNU_PLEDGE_MOUNT 0x0000000400000000ULL
#define FNU_PLEDGE_UNVEIL 0x0000004000000000ULL
#define FNU_PLEDGE_ERROR 0x0000008000000000ULL

struct fnu_segment {
  void *base;
  uint64 len;
};

#define FNU_FD_CLOSEEXEC 0x1
#define FNU_FD_NOWAIT 0x2

#define FNU_DUP_EXACT 0x100

struct fnu_fdstate {
  uint32 fdflags;
  uint32 fileflags;
};

#define FNU_LOG_CONSOLE 0x02
#define FNU_LOG_MAXLINE 8192

#define FNU_MEMUSE_NORMAL 0
#define FNU_MEMUSE_RANDOM 1
#define FNU_MEMUSE_SEQUENTIAL 2
#define FNU_MEMUSE_WILLNEED 3
#define FNU_MEMUSE_DONTNEED 4
#define FNU_MEMUSE_FREE 5

#define FNU_MEMSYNC_WAIT 0x02
#define FNU_MEMSYNC_START 0x01

#define FNU_MEM_FAILED ((void *)-1)

#define FNU_SIGSTACK_INUSE 0x1
#define FNU_SIGSTACK_OFF 0x4
#define FNU_SIGSTACK_MIN 8192

#define FNU_HALT_STOP 0
#define FNU_HALT_POWEROFF 1
#define FNU_HALT_REBOOT 2
#define FNU_HALT_FORCE 0x100

struct fnu_altstack {
  uint64_t base;
  uint64_t size;
  uint32_t flags;
  uint32_t pad;
};

// NETWORK ENDPOINTS
#define FNU_NET_ANY 0
#define FNU_NET_LOCAL 1
#define FNU_NET_INET 2
#define FNU_NET_INET6 3

#define FNU_SOCKET_STREAM 1
#define FNU_SOCKET_DATAGRAM 2
#define FNU_SOCKET_RAW 3 // root's
#define FNU_PROTO_ICMP 1
#define FNU_PROTO_TCP 6
#define FNU_PROTO_UDP 17
#define FNU_SOCKET_NOWAIT 0x100
#define FNU_SOCKET_CLOSEEXEC 0x200

#define FNU_NETADDR_MAX 255

#define FNU_SHUTDOWN_READ 0x1
#define FNU_SHUTDOWN_WRITE 0x2
#define FNU_SHUTDOWN_BOTH (FNU_SHUTDOWN_READ | FNU_SHUTDOWN_WRITE)

#define FNU_SOCKOPT_REUSEADDR 1
#define FNU_SOCKOPT_TYPE 2
#define FNU_SOCKOPT_ERROR 3
#define FNU_SOCKOPT_RECVTIME 4 // fnu_timespec, 0 for ever
#define FNU_SOCKOPT_SENDTIME 5
#define FNU_SOCKOPT_PEER 6

#define FNU_SOCKADDR_LOCAL 0
#define FNU_SOCKADDR_PEER 1

struct fnu_peer {
  uint32 uid;
  uint32 gid;
  int32 pid;
};

struct fnu_localaddr {
  uint8 len;
  uint8 family;
  char path[104];
};

struct fnu_inetaddr {
  uint8 len;
  uint8 family;
  uint16 port; // network order
  uint8 addr[4];
  uint8 zero[8];
};

#define FNU_RECV_PEEK 0x01
#define FNU_RECV_ALL 0x02
#define FNU_MSG_NOWAIT 0x04
#define FNU_SEND_NOSIGNAL 0x08
#define FNU_SEND_LOCALNET 0x10
#define FNU_RECV_FDCLOSEEXEC 0x20
#define FNU_RECV_CUT 0x100
#define FNU_RECV_FDCUT 0x200

struct fnu_msg {
  struct fnu_segment *iov;
  uint32 iovcnt;
  uint32 flags;
  void *addr;
  uint32 addrlen;
  uint32 nfds;
  int32 *fds;
};

#define FNU_PT_TRACE_ME 0
#define FNU_PT_READ_I 1
#define FNU_PT_READ_D 2
#define FNU_PT_WRITE_I 4
#define FNU_PT_WRITE_D 5
#define FNU_PT_CONTINUE 7
#define FNU_PT_KILL 8
#define FNU_PT_ATTACH 9
#define FNU_PT_DETACH 10
#define FNU_PT_IO 11
#define FNU_PT_SYSCALL 14
#define FNU_PT_SYSCALLEMU 15
#define FNU_PT_SET_EVENT_MASK 16
#define FNU_PT_GET_EVENT_MASK 17
#define FNU_PT_GET_PROCESS_STATE 18
#define FNU_PT_FIRSTMACH 32
#define FNU_PT_STEP (FNU_PT_FIRSTMACH + 0)
#define FNU_PT_GETREGS (FNU_PT_FIRSTMACH + 1)
#define FNU_PT_SETREGS (FNU_PT_FIRSTMACH + 2)
#define FNU_PT_GETFPREGS (FNU_PT_FIRSTMACH + 3)
#define FNU_PT_SETFPREGS (FNU_PT_FIRSTMACH + 4)
#define FNU_PT_GETDBREGS (FNU_PT_FIRSTMACH + 5)
#define FNU_PT_SETDBREGS (FNU_PT_FIRSTMACH + 6)

struct fnu_ptrace_event {
  int32_t pe_set_event;
};

struct fnu_ptrace_state {
  int32_t pe_report_event;
  int32_t pe_other_pid;
};

#define FNU_PTRACE_FORK 0x0001
#define FNU_PTRACE_EXEC 0x0100
#define FNU_PTRACE_SCE 0x0200
#define FNU_PTRACE_SCX 0x0400
#define FNU_PTRACE_HWBP 0x0800

struct fnu_dbregs {
  uint64_t dr[8];
};

struct fnu_fpregs {
  uint8_t image[512];
};

#define FNU_PT_CONTINUE_HERE 1

struct fnu_regs {
  uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip, rflags, rsp;
  uint64_t cs, ss;
};

#define FNU_PT_IO_READ 0
#define FNU_PT_IO_WRITE 1

struct fnu_ptrace_io {
  uint32 op;
  uint32 len;
  uint64_t addr;
  uint64_t buf;
};

// THREADS
struct fnu_thread_start {
  uint64_t entry;
  uint64_t arg;
  uint64_t stack;
  uint64_t tcb;
  uint64_t tid;
};

#define FNU_FUTEX_WAIT 1
#define FNU_FUTEX_WAKE 2
#define FNU_FUTEX_REQUEUE 3
#define FNU_FUTEX_OP_MASK 0x7f
#define FNU_FUTEX_PRIVATE 0x80
#define FNU_FUTEX_FOREVER (~(uint64_t)0)

// SYS_gettime
#define FNU_CLOCK_WALL 0
#define FNU_CLOCK_UPTIME 1
#define FNU_CLOCK_GRAIN 0x100

// SYS_ready
#define FNU_READY_IN 0x0001
#define FNU_READY_OUT 0x0002
#define FNU_READY_PRI 0x0004
#define FNU_READY_HUP 0x0010
#define FNU_READY_ERR 0x0020
#define FNU_READY_BAD 0x0040
#define FNU_READY_MAX 64
#define FNU_READY_LIMIT 4096

struct fnu_ready {
  int32    fd;
  uint16   want;
  uint16   got;
};

// fnu_events
#define FNU_EVF_READ 1
#define FNU_EVF_WRITE 2
#define FNU_EVF_FILE 3
#define FNU_EVF_PROC 4
#define FNU_EVF_SIGNAL 5
#define FNU_EVF_TIMER 6

#define FNU_EV_ADD 0x0001
#define FNU_EV_DELETE 0x0002
#define FNU_EV_ENABLE 0x0004
#define FNU_EV_DISABLE 0x0008
#define FNU_EV_ONESHOT 0x0010
#define FNU_EV_CLEAR 0x0020
#define FNU_EV_RECEIPT 0x0040
#define FNU_EV_DISPATCH 0x0080
#define FNU_EV_ERROR 0x4000
#define FNU_EV_EOF 0x8000

// FNU_EVF_FILE
#define FNU_NOTE_DELETE 0x0001
#define FNU_NOTE_WRITE 0x0002
#define FNU_NOTE_EXTEND 0x0004
#define FNU_NOTE_ATTRIB 0x0008
#define FNU_NOTE_LINK 0x0010
#define FNU_NOTE_RENAME 0x0020
#define FNU_NOTE_TRUNCATE 0x0080
// FNU_EVF_PROC
#define FNU_NOTE_EXIT 0x80000000
#define FNU_NOTE_FORK 0x40000000
#define FNU_NOTE_EXEC 0x20000000
#define FNU_NOTE_SECONDS 0x0001
#define FNU_NOTE_MSECONDS 0x0002
#define FNU_NOTE_USECONDS 0x0004
#define FNU_NOTE_NSECONDS 0x0008

#define FNU_EVENTS_MAX 4096

struct fnu_event {
  uint64 ident;
  int16_t filter;
  uint16 flags;
  uint32 fflags;
  int64 data;
  uint64 udata;
};

#define FNU_SEL_READ FNU_READY_IN
#define FNU_SEL_WRITE FNU_READY_OUT
#define FNU_SEL_ERROR 4

#define FNU_HZ 100

struct fnu_wsevent {
  uint32 type;
  int32_t value;
  struct fnu_timespec time;
};

#define FNU_WSEVENT_MOUSE_UP 4
#define FNU_WSEVENT_MOUSE_DOWN 5
#define FNU_WSEVENT_MOUSE_DELTA_X 6
#define FNU_WSEVENT_MOUSE_DELTA_Y 7
#define FNU_WSEVENT_MOUSE_DELTA_Z 10
#define FNU_WSEVENT_MOUSE_ABSOLUTE_X 8
#define FNU_WSEVENT_MOUSE_ABSOLUTE_Y 9
#define FNU_WSEVENT_TOUCH_PRESSURE 11
#define FNU_WSEVENT_TOUCH_CONTACTS 17
#define FNU_WSEVENT_SYNC 18
#define FNU_WSEVENT_TOUCH_WIDTH 24
#define FNU_WSEVENT_TOUCH_RESET 25

#define FNU_WSMOUSE_TYPE_PS2 2
#define FNU_WSMOUSE_TYPE_USB 3
#define FNU_WSMOUSE_TYPE_SYNAPTICS 15
#define FNU_WSMOUSE_TYPE_ELANTECH 18
#define FNU_WSMOUSE_TYPE_TOUCHPAD 20
#define FNU_WSMOUSE_COMPAT 0
#define FNU_WSMOUSE_NATIVE 1

struct fnu_wsmouse_calib {
  int32 minx, miny;
  int32 maxx, maxy;
  int32 swapxy;
  int32 resx, resy;
};

#define FNU_WSMOUSEGTYPE _IOR('W', 32, uint32)
#define FNU_WSMOUSEGCALIB _IOR('W', 37, struct fnu_wsmouse_calib)
#define FNU_WSMOUSESETMODE _IOW('W', 38, int)

#define FNU_TIMER_REAL 0
#define FNU_TIMER_VIRTUAL 1
#define FNU_TIMER_PROFILE 2
#define FNU_TIMER_COUNT 3

struct fnu_timer {
  struct fnu_timespec value;
  struct fnu_timespec interval;
};

// fnu_niceness
#define FNU_NICE_PROCESS 0
#define FNU_NICE_GROUP 1
#define FNU_NICE_USER 2
#define FNU_NICE_GET 0
#define FNU_NICE_SET 1
#define FNU_NICE_ADD 2
#define FNU_NICE_MIN (-20)
#define FNU_NICE_MAX 19

#define FNU_LIMIT_CPU 0
#define FNU_LIMIT_FILESIZE 1
#define FNU_LIMIT_DATA 2
#define FNU_LIMIT_STACK 3
#define FNU_LIMIT_CORE 4
#define FNU_LIMIT_RSS 5
#define FNU_LIMIT_MEMLOCK 6
#define FNU_LIMIT_PROCS 7
#define FNU_LIMIT_OPENFILES 8
#define FNU_LIMIT_COUNT 9

#define FNU_LIMIT_NONE (~(uint64_t)0)

struct fnu_limit {
  uint64_t cur;
  uint64_t max;
};

// getrusage(2)
#define FNU_USAGE_SELF 0
#define FNU_USAGE_CHILDREN (-1)

// SYS_rtctime, binary not BCD
struct fnu_rtctime {
  uint8  sec;
  uint8  min;
  uint8  hour;
  uint8  wday;
  uint8  mday;
  uint8  mon;
  uint16 year;
};

#define FNU_MNT_NAMELEN 128
#define FNU_MNT_TYPELEN 8

// SYS_hwinfo
#define FNU_HW_BRAND_MAX  49
#define FNU_HW_VENDOR_MAX 13
struct fnu_hwinfo {
  char     cpu_brand[FNU_HW_BRAND_MAX];
  char     cpu_vendor[FNU_HW_VENDOR_MAX];
  uint8    pad0[2];
  uint32   cpu_count;
  uint32   cpu_family;
  uint32   cpu_model;
  uint32   cpu_stepping;
  uint64   tsc_hz;
  uint64   cpu_base_hz;
  uint64   cpu_max_hz;
  uint32   fb_width;
  uint32   fb_height;
  uint32   fb_bpp;
  uint32   fb_columns;
  uint32   fb_rows;
  uint32   pci_devices;
  uint32   pci_total;
  uint32   fb_modeset;
  uint32   fb_source;
  uint32   pad1;
  uint64   fb_vram;
  uint64   ram_total;
  uint64   ram_free;
};

// SYS_diskinfo
#define FNU_DISK_MODEL_MAX   41
#define FNU_DISK_SERIAL_MAX  41
#define FNU_DISK_NONE    0
#define FNU_DISK_SATA    1
#define FNU_DISK_NVME    2
#define FNU_DISK_RAMDISK 3
#define FNU_DISK_IDE     4
#define FNU_DISK_USB     5
struct fnu_diskinfo {
  char     model[FNU_DISK_MODEL_MAX];
  char     serial[FNU_DISK_SERIAL_MAX];
  uint8    kind;
  uint8    pad0[1];
  uint32   sector_size;
  uint32   link_gen;
  uint32   pad1;
  uint64   sectors;
};

#define FNU_MNT_RDONLY 0x0001
#define FNU_MNT_FORCE  0x0002
#define FNU_MNT_NOSUID 0x0004
#define FNU_MNT_LOCAL 0x0008
#define FNU_MNT_UPDATE 0x0010

struct fnu_mount_req {
  char   from[FNU_MNT_NAMELEN];
  char   on[FNU_MNT_NAMELEN];
  char   type[FNU_MNT_TYPELEN];
  uint32 flags;
  uint32 pad;
};

// SYS_fsstat
struct fnu_fsstat {
  char   from[FNU_MNT_NAMELEN];
  char   on[FNU_MNT_NAMELEN];
  char   type[FNU_MNT_TYPELEN];
  uint32 flags;
  uint32 dev;
  uint32 bsize;   // a fragment
  uint32 iosize;
  uint32 namemax;
  uint32 pad;
  uint64 blocks;
  uint64 bfree;
  int64  bavail;
  uint64 files;
  uint64 ffree;
  int64  favail;
};

#define FNU_HOTPLUG_ATTACH 1
#define FNU_HOTPLUG_DETACH 2

#define FNU_DEVCLASS_OTHER   0
#define FNU_DEVCLASS_DISK    1
#define FNU_DEVCLASS_KEYBOARD 2
#define FNU_DEVCLASS_MOUSE   3
#define FNU_DEVCLASS_NET     4
#define FNU_DEVCLASS_AUDIO   5
#define FNU_DEVCLASS_TERMINAL 6
#define FNU_DEVCLASS_MEMORY  7

struct fnu_audioinfo {
  uint32 present;
  uint32 rate;
  uint32 channels;
  uint32 width;
  uint32 interval;
  uint32 queued;
  uint32 capacity;
  uint32 starved;
  uint32 rec_present;
  uint32 rec_rate;
  uint32 rec_channels;
  uint32 rec_width;
  uint32 rec_queued;
  uint32 rec_overruns;
  uint32 intervals;
  uint32 rec_intervals;
};

// sys/audioio.h; ~0 not asked about
struct fnu_audio_swpar {
  uint32 sig;
  uint32 le;
  uint32 bits;
  uint32 bps;
  uint32 msb;
  uint32 rate;
  uint32 pchan;
  uint32 rchan;
  uint32 nblks;
  uint32 round;
  uint32 _spare[6];
};
#define FNU_AUDIO_INITPAR(p) memset((p), 0xff, sizeof(struct fnu_audio_swpar))
#define FNU_AUDIOGETPAR _IOR('A', 36, struct fnu_audio_swpar)
#define FNU_AUDIOSETPAR _IOWR('A', 37, struct fnu_audio_swpar)
#define FNU_AUDIOGETINFO _IOR('A', 64, struct fnu_audioinfo)

#define FNU_HOTPLUG_NAMELEN 16

#define FNU_HOTPLUG_REMOVABLE 0x0001

struct fnu_hotplug_event {
  uint32 type;
  uint32 devclass;
  uint32 flags;
  uint32 dev;
  char   name[FNU_HOTPLUG_NAMELEN];
};

// pathconf(2)
#define FNU_PC_LINK_MAX 1
#define FNU_PC_NAME_MAX 4
#define FNU_PC_PATH_MAX 5
#define FNU_PC_PIPE_BUF 6
#define FNU_PC_CHOWN_RESTRICTED 7
#define FNU_PC_NO_TRUNC 8
#define FNU_PC_2_SYMLINKS 10
#define FNU_PC_ALLOC_SIZE_MIN 11
#define FNU_PC_FILESIZEBITS 13
#define FNU_PC_SYMLINK_MAX 19
#define FNU_PC_TIMESTAMP_RESOLUTION 21

#define FNU_USER_NAME_MAX 16
struct fnu_user_info {
  char name[FNU_USER_NAME_MAX];
  uint32 uid;
  uint32 gid;
  uint32 wheel;
};

#endif
