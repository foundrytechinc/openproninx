#ifndef PRONINX_USB_USBDI_H
#define PRONINX_USB_USBDI_H

#include "param.h"
#include "usb.h"
#include "unit.h"

struct usb_bus;
struct usb_device;
struct usb_pipe;
struct usb_attach_arg;

#define USB_CDESC_MAX 1024
#define USB_MAX_STRING_LEN 128
#define USB_RESUME_MS 40          // USB_RESUME_TIMEOUT: resume signalling held
#define USB_RESUME_RECOVERY_MS 10 // TRSMRCY
#define USB_RDESC_MAX 512
#define USB_XFER_MAX MAXPHYS

#define USB_ISOC_SEGS 32
#define USB_ISOC_STRIDE 4096

#define USBD_NORMAL 0
#define USBD_SHORT 1
#define USBD_STALLED 2
#define USBD_TIMEOUT 3
#define USBD_IOERROR 4
#define USBD_CANCELLED 5

struct usb_pipe {
  struct usb_device *dev;
  struct usb_endpoint_descriptor edesc;
  uint16_t maxp;
  uint16_t bpi;
  uint8_t burst;
  void *hcpriv;
  int open;

  void (*done)(struct usb_pipe *, void *arg, int status, int len);
  void *arg;
  volatile int sync_done;
  volatile int sync_status;
  volatile int sync_len;
  uint8_t *buf;
  uint64_t buf_phys;
  int buflen;
  int xfer_len;
  int repeat;
  int polled;

  int isoc;
  int nseg;
  int seglen;
  int seg;
  int seg_next;
};

struct usb_bus_methods {
  int (*new_device)(struct usb_device *);
  void (*free_device)(struct usb_device *);
  int (*open_pipe)(struct usb_pipe *);
  void (*close_pipe)(struct usb_pipe *);
  int (*control)(struct usb_device *, const struct usb_device_request *,
                 void *data, int *len);
  int (*start)(struct usb_pipe *);
  int (*transfer)(struct usb_pipe *, int len, int ms);
  int (*isoc_start)(struct usb_pipe *);
  int (*set_maxp0)(struct usb_device *);
  int (*set_hub)(struct usb_device *, int nports, int tt_think);
  void (*abort_device)(struct usb_device *);
  void (*clear_toggle)(struct usb_pipe *); // back to DATA0
  int (*root_reset)(struct usb_bus *, int port);
  int (*readdress)(struct usb_device *); // after a port reset
  void (*poll)(struct usb_bus *);
  // 0: devices kept state; else reset and restart
  void (*suspend)(struct usb_bus *);
  int (*resume)(struct usb_bus *, int lost);
  void (*restart)(struct usb_bus *);
};

struct usb_bus {
  const struct usb_bus_methods *methods;
  void *hcpriv;
  const char *name;
  struct usb_device *root;
  struct usb_bus *next;
  int explore_wanted;
};

struct usb_attachment {
  struct usb_attachment *next;
  const struct usb_driver *driver;
  void *softc;
  int iface;
};

struct usb_device {
  struct usb_bus *bus;
  struct usb_device *parent;
  struct usb_device *hs_hub;
  uint8_t hs_port;
  uint8_t address;
  uint8_t speed;
  uint8_t depth;
  uint8_t port;
  uint8_t rhport;
  uint32_t route;
  uint16_t maxp0;
  void *hcpriv;

  struct usb_device_descriptor ddesc;
  uint8_t cdesc[USB_CDESC_MAX];
  int cdesc_len;
  uint8_t config;

  struct usb_pipe ep0;

  int nports;
  struct usb_device **child;
  int (*explore)(struct usb_device *);
  void *hubsoftc;
  uint64_t powered_at; // ticks: a hub's ports had their power again

  struct usb_attachment *attached;
  volatile int gone;
  int in_use;
  char name[16];
  int langid; // USBD_NOLANG until asked
  char serial[USB_MAX_STRING_LEN];
};

struct usb_attach_arg {
  struct usb_device *dev;
  int iface;
  const struct usb_interface_descriptor *idesc;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t proto;
  uint16_t vendor;
  uint16_t product;
  void *softc;
};

struct usb_driver {
  const char *name;
  int (*match)(struct usb_attach_arg *);
  int (*attach)(struct usb_attach_arg *);
  void (*detach)(struct usb_device *, void *softc);
  void (*media)(struct usb_device *, void *softc);
  void (*service)(struct usb_device *, void *softc);
  void (*woke)(void);
};

#define USB_MATCH_NONE 0
#define USB_MATCH_IFACE_CLASS 10
#define USB_MATCH_DEV_CLASS 20
#define USB_MATCH_VENDOR_PRODUCT 30

void usbd_init(void);
void usbd_poll_all(void);
extern int usbd_polling;
void usb_pause_ms(uint ms);
int usbd_register_driver(const struct usb_driver *drv);
void usbd_attach_bus(struct usb_bus *bus);
int usbd_hub_ports(struct usb_device *dev, int nports);

int usbd_new_device(struct usb_bus *bus, struct usb_device *parent, int depth,
                    int speed, int port, struct usb_device **out);
void usbd_free_device(struct usb_device *dev);

int usbd_do_request(struct usb_device *dev, uint8_t type, uint8_t request,
                    uint16_t value, uint16_t index, uint16_t len, void *data);
int usbd_get_string(struct usb_device *dev, int si, char *buf, int buflen);
int usbd_get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                        uint16_t len, void *data);
int usbd_set_config(struct usb_device *dev, uint8_t config);
int usbd_set_interface(struct usb_device *dev, int iface, int alt);
int usbd_clear_endpoint_stall(struct usb_pipe *pipe);
int usbd_reset_device(struct usb_device *dev, struct usb_pipe **pipes, int n);

// device quirks, and hw.usb.quirk.N
#define UQ_NO_SYNC_CACHE 0x1
#define UQ_NO_TEST_UNIT_READY 0x2
#define UQ_NO_GETMAXLUN 0x4
#define UQ_NO_START_STOP 0x8
#define UQ_VENDOR_ONLY 0x80000000u
uint32_t usbd_quirks(const struct usb_device *dev);
int uhub_port_reset(struct usb_device *hub, int port);

const struct usb_interface_descriptor *usbd_find_iface(struct usb_device *dev,
                                                       int index);
const struct usb_endpoint_descriptor *
usbd_find_endpoint(struct usb_device *dev, int iface, int index);
const struct usb_interface_descriptor *
usbd_find_altiface(struct usb_device *dev, int ifaceno, int alt);
const struct usb_endpoint_descriptor *
usbd_find_alt_endpoint(struct usb_device *dev, int ifaceno, int alt, int index);
const void *usbd_find_alt_class_desc(struct usb_device *dev, int ifaceno,
                                     int alt, uint8_t type, int index);
int usbd_hid_report_desc(struct usb_device *dev, int iface, uint8_t *buf,
                         int max);
const void *usbd_find_class_desc(struct usb_device *dev, int iface,
                                 uint8_t type);

int usbd_open_pipe(struct usb_device *dev,
                   const struct usb_endpoint_descriptor *ed,
                   struct usb_pipe *pipe);
void usbd_close_pipe(struct usb_pipe *pipe);
int usbd_start_pipe(struct usb_pipe *pipe, int len,
                    void (*done)(struct usb_pipe *, void *, int, int),
                    void *arg);
int usbd_bulk_transfer(struct usb_pipe *pipe, void *data, int len, int ms);
int usbd_isoc_start(struct usb_pipe *pipe, int seglen, int nseg,
                    void (*done)(struct usb_pipe *, void *, int, int),
                    void *arg);
static inline uint8_t *usbd_isoc_seg(struct usb_pipe *pipe, int seg) {
  return pipe->buf + (uint32_t)seg * USB_ISOC_STRIDE;
}

void usbd_needs_explore(struct usb_bus *bus);
void usbd_needs_service(void);
void usbd_explore_thread(void *);
int usbd_sleeping(void);
void usbd_suspend(void);
void usbd_resume(int lost);
const char *usbd_speed_name(int speed);

#endif
