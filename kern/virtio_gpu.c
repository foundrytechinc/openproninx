#include "virtio_gpu.h"
#include "defs.h"
#include "driver.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "mutex.h"
#include "x86.h"

struct vtgpu_softc {
  struct device *dev;
  struct mutex lock;
  volatile struct virtio_pci_common_cfg *common;
  volatile uint16_t *notify;
  uint8_t *notify_base;
  uint32_t notify_mult;
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;
  uint16_t last_used;
  uchar *pixels;
  uint64_t phys;
  uint width, height, pitch;
};

static struct vtgpu_softc *console_gpu;

int virtio_gpu_available(void) { return console_gpu != 0; }

void *virtio_gpu_framebuffer(void) { return console_gpu->pixels; }

uint virtio_gpu_width(void) { return console_gpu->width; }

uint virtio_gpu_height(void) { return console_gpu->height; }

uint virtio_gpu_pitch(void) { return console_gpu->pitch; }

static int virtio_gpu_exec(struct vtgpu_softc *sc, void *cmd, uint32_t cmd_sz,
                           void *resp, uint32_t resp_sz) {
  uint16_t avail_idx;
  int timeout;

  mtx_enter(&sc->lock);
  sc->desc[0].address = V2P(cmd);
  sc->desc[0].length = cmd_sz;
  sc->desc[0].flags = 1; // VRING_DESC_F_NEXT
  sc->desc[0].next = 1;
  sc->desc[1].address = V2P(resp);
  sc->desc[1].length = resp_sz;
  sc->desc[1].flags = 2; // VRING_DESC_F_WRITE
  sc->desc[1].next = 0;
  avail_idx = sc->avail->index;
  sc->avail->ring[avail_idx % VTGPU_QSIZE] = 0;
  mb();
  sc->avail->index = avail_idx + 1;
  mb();
  *sc->notify = 0;
  for (timeout = 100000; sc->used->index == sc->last_used && --timeout > 0;)
    delay(10);
  if (timeout == 0) {
    mtx_leave(&sc->lock);
    printf("%s: command timeout\n", sc->dev->name);
    return -1;
  }
  sc->last_used = sc->used->index;
  mtx_leave(&sc->lock);
  return 0;
}

void virtio_gpu_flush_rect(uint x, uint y, uint w, uint h) {
  struct vtgpu_softc *sc = console_gpu;

  if (sc == 0 || w == 0 || h == 0 || x >= sc->width || y >= sc->height)
    return;

  if (x + w > sc->width)
    w = sc->width - x;
  if (y + h > sc->height)
    h = sc->height - y;

  struct virtio_gpu_transfer_to_host_2d cmd_xfer;
  struct virtio_gpu_resource_flush cmd_flush;
  struct virtio_gpu_ctrl_hdr resp;

  memset(&cmd_xfer, 0, sizeof(cmd_xfer));
  cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  cmd_xfer.resource_id = 1;
  cmd_xfer.offset = ((uint64_t)y * sc->pitch) + (x * 4);
  cmd_xfer.rect.x = x;
  cmd_xfer.rect.y = y;
  cmd_xfer.rect.width = w;
  cmd_xfer.rect.height = h;
  virtio_gpu_exec(sc, &cmd_xfer, sizeof(cmd_xfer), &resp, sizeof(resp));

  memset(&cmd_flush, 0, sizeof(cmd_flush));
  cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  cmd_flush.resource_id = 1;
  cmd_flush.rect.x = x;
  cmd_flush.rect.y = y;
  cmd_flush.rect.width = w;
  cmd_flush.rect.height = h;
  virtio_gpu_exec(sc, &cmd_flush, sizeof(cmd_flush), &resp, sizeof(resp));
}

void virtio_gpu_flush_all(void) {
  if (console_gpu != 0)
    virtio_gpu_flush_rect(0, 0, console_gpu->width, console_gpu->height);
}

static int virtio_gpu_probe(struct device *dev) {
  if (dev->pci.vendor_id == 0x1af4 && dev->pci.device_id == 0x1050)
    return 10;
  return 0;
}

static uint8_t *bar_map(const struct pci_device *p, uint8_t b, uint32_t off) {
  uint64_t len = p->bar_size[b];

  if (len < (uint64_t)off + 0x1000)
    len = (uint64_t)off + 0x1000;
  return (uint8_t *)ioremap(p->bar[b], len) + off;
}

static int gpu_device_up(struct vtgpu_softc *sc) {
  volatile struct virtio_pci_common_cfg *c = sc->common;
  uint32_t f1;

  c->device_status = 0;
  mb();
  c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
  mb();

  c->device_feature_select = 1;
  f1 = c->device_feature;
  c->driver_feature_select = 1;
  c->driver_feature = f1 & (1U << (VIRTIO_F_VERSION_1 - 32));
  c->device_feature_select = 0;
  c->driver_feature_select = 0;
  c->driver_feature = 0;
  mb();

  c->device_status |= VIRTIO_STATUS_FEATURES_OK;
  mb();
  if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK)) {
    printf("%s: FEATURES_OK rejected\n", sc->dev->name);
    return -1;
  }

  c->queue_select = 0;
  if (c->queue_size < VTGPU_QSIZE) {
    printf("%s: a control queue of %d\n", sc->dev->name, c->queue_size);
    return -1;
  }
  c->queue_size = VTGPU_QSIZE;
  memset(sc->desc, 0, PGSIZE);
  sc->last_used = 0;
  c->queue_desc = V2P(sc->desc);
  c->queue_driver = V2P(sc->avail);
  c->queue_device = V2P(sc->used);
  c->queue_enable = 1;
  mb();
  sc->notify = (volatile uint16_t *)(sc->notify_base +
                                     c->queue_notify_off * sc->notify_mult);
  c->device_status |= VIRTIO_STATUS_DRIVER_OK;
  mb();
  return 0;
}

static int gpu_cmd_ok(struct vtgpu_softc *sc, void *cmd, uint32_t len) {
  struct virtio_gpu_ctrl_hdr resp;

  return virtio_gpu_exec(sc, cmd, len, &resp, sizeof(resp)) == 0 &&
                 resp.type == VIRTIO_GPU_RESP_OK_NODATA
             ? 0
             : -1;
}

static int gpu_resource_up(struct vtgpu_softc *sc, uint w, uint h) {
  struct virtio_gpu_resource_create_2d cmd_create;
  struct virtio_gpu_resource_attach_backing cmd_attach;
  struct virtio_gpu_set_scanout cmd_scanout;

  memset(&cmd_create, 0, sizeof(cmd_create));
  cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  cmd_create.resource_id = 1;
  cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  cmd_create.width = w;
  cmd_create.height = h;
  memset(&cmd_attach, 0, sizeof(cmd_attach));
  cmd_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  cmd_attach.resource_id = 1;
  cmd_attach.nr_entries = 1;
  cmd_attach.entries[0].addr = sc->phys;
  cmd_attach.entries[0].length = (w * 4 * h + 4095) & ~4095U;
  memset(&cmd_scanout, 0, sizeof(cmd_scanout));
  cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  cmd_scanout.scanout_id = 0;
  cmd_scanout.resource_id = 1;
  cmd_scanout.rect.width = w;
  cmd_scanout.rect.height = h;
  if (gpu_cmd_ok(sc, &cmd_create, sizeof(cmd_create)) < 0 ||
      gpu_cmd_ok(sc, &cmd_attach, sizeof(cmd_attach)) < 0 ||
      gpu_cmd_ok(sc, &cmd_scanout, sizeof(cmd_scanout)) < 0)
    return -1;
  return 0;
}

static void gpu_resource_down(struct vtgpu_softc *sc) {
  struct virtio_gpu_resource_detach_backing cmd_detach;
  struct virtio_gpu_resource_unref cmd_unref;

  memset(&cmd_detach, 0, sizeof(cmd_detach));
  cmd_detach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
  cmd_detach.resource_id = 1;
  gpu_cmd_ok(sc, &cmd_detach, sizeof(cmd_detach));
  memset(&cmd_unref, 0, sizeof(cmd_unref));
  cmd_unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
  cmd_unref.resource_id = 1;
  gpu_cmd_ok(sc, &cmd_unref, sizeof(cmd_unref));
}

static int gpu_display_size(struct vtgpu_softc *sc, uint *w, uint *h) {
  struct virtio_gpu_ctrl_hdr cmd_info;
  struct virtio_gpu_resp_display_info resp_info;
  int s;

  memset(&cmd_info, 0, sizeof(cmd_info));
  cmd_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
  memset(&resp_info, 0, sizeof(resp_info));
  if (virtio_gpu_exec(sc, &cmd_info, sizeof(cmd_info), &resp_info,
                      sizeof(resp_info)) == 0 &&
      resp_info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    for (s = 0; s < VIRTIO_GPU_MAX_SCANOUTS; s++)
      if (resp_info.pmodes[s].enabled && resp_info.pmodes[s].rect.width > 0 &&
          resp_info.pmodes[s].rect.height > 0) {
        *w = resp_info.pmodes[s].rect.width;
        *h = resp_info.pmodes[s].rect.height;
        return 0;
      }
  *w = framebuffer_boot_width();
  *h = framebuffer_boot_height();
  return *w != 0 && *h != 0 ? 0 : -1;
}

static int gpu_mode(struct vtgpu_softc *sc, uint w, uint h) {
  uint64_t bytes = (uint64_t)w * 4 * h;
  uchar *pixels, *old = sc->pixels;

  if (w < 320 || h < 200 || bytes > 0xffffffffull)
    return -1;
  if ((pixels = contig_alloc(bytes)) == 0)
    return -1;
  memset(pixels, 0, bytes);
  if (old != 0)
    gpu_resource_down(sc);
  sc->pixels = pixels;
  sc->phys = V2P(pixels);
  if (gpu_resource_up(sc, w, h) < 0) {
    sc->pixels = old;
    sc->phys = old != 0 ? V2P(old) : 0;
    contig_free(pixels);
    if (old != 0)
      gpu_resource_up(sc, sc->width, sc->height);
    return -1;
  }
  if (old != 0)
    contig_free(old);
  sc->width = w;
  sc->height = h;
  sc->pitch = w * 4;
  return 0;
}

static int virtio_gpu_attach(struct device *dev) {
  struct pci_device *p = &dev->pci;
  struct vtgpu_softc *sc;
  uint8_t cap_ptr, common_bar = 0xff, notify_bar = 0xff;
  uint32_t common_off = 0, notify_off = 0, notify_mult = 0;
  uint w, h;
  char *q;

  if (console_gpu != 0) {
    printf("%s: %s shows the console already\n", dev->name, console_gpu->dev->name);
    return -1;
  }
  pci_enable_bus_master(p);
  for (cap_ptr = pci_read8(p->bus, p->device, p->function, 0x34); cap_ptr != 0;
       cap_ptr = pci_read8(p->bus, p->device, p->function, cap_ptr + 1)) {
    uint8_t cfg_type, bar;
    uint32_t off;

    if (pci_read8(p->bus, p->device, p->function, cap_ptr) != 0x09)
      continue;
    cfg_type = pci_read8(p->bus, p->device, p->function, cap_ptr + 3);
    bar = pci_read8(p->bus, p->device, p->function, cap_ptr + 4);
    off = pci_read32(p->bus, p->device, p->function, cap_ptr + 8);
    if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
      common_bar = bar;
      common_off = off;
    } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
      notify_bar = bar;
      notify_off = off;
      notify_mult = pci_read32(p->bus, p->device, p->function, cap_ptr + 16);
    }
  }
  if (common_bar >= 6 || notify_bar >= 6 || p->bar[common_bar] == 0 ||
      p->bar[notify_bar] == 0) {
    printf("%s: required PCI capabilities missing\n", dev->name);
    return -1;
  }
  if ((q = page_alloc()) == 0)
    return -1;
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->dev = dev;
  mtx_init(&sc->lock, "vtgpu");
  sc->common = (volatile struct virtio_pci_common_cfg *)bar_map(p, common_bar, common_off);
  sc->notify_base = bar_map(p, notify_bar, notify_off);
  sc->notify_mult = notify_mult;
  sc->desc = (struct vring_desc *)q;
  sc->avail = (struct vring_avail *)(q + VTGPU_QSIZE * sizeof(struct vring_desc));
  sc->used = (struct vring_used *)(q + PGSIZE / 2);
  if (gpu_device_up(sc) < 0 || gpu_display_size(sc, &w, &h) < 0 ||
      gpu_mode(sc, w, h) < 0) {
    printf("%s: no display could be made\n", dev->name);
    sc->common->device_status = 0;
    page_free(q);
    free(sc, M_DEVBUF);
    return -1;
  }
  dev->driver_data = sc;
  console_gpu = sc;
  virtio_gpu_flush_all();
  framebuffer_init_gpu(sc->pixels, sc->width, sc->height, sc->pitch);
  console_switch_to_gpu();
  printf("%s: %dx%d, 32 bit\n", dev->name, sc->width, sc->height);
  return 0;
}

int virtio_gpu_set_resolution(uint w, uint h) {
  struct vtgpu_softc *sc = console_gpu;

  if (sc == 0 || gpu_mode(sc, w, h) < 0)
    return -1;
  virtio_gpu_flush_all();
  framebuffer_init_gpu(sc->pixels, sc->width, sc->height, sc->pitch);
  console_switch_to_gpu();
  printf("%s: resolution changed to %dx%d\n", sc->dev->name, sc->width,
          sc->height);
  return 0;
}

// SLEEP
static int virtio_gpu_suspend(struct device *dev) {
  (void)dev;
  return 0;
}

static int virtio_gpu_resume(struct device *dev) {
  struct vtgpu_softc *sc = dev->driver_data;

  if (gpu_device_up(sc) < 0 || gpu_resource_up(sc, sc->width, sc->height) < 0)
    return -1;
  virtio_gpu_flush_all();
  return 0;
}

static struct driver virtio_gpu_driver = {
    .name = "vtgpu",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_DISPLAY,
    .probe = virtio_gpu_probe,
    .attach = virtio_gpu_attach,
    .suspend = virtio_gpu_suspend,
    .resume = virtio_gpu_resume,
};

int virtio_gpu_driver_init(void) { return driver_register(&virtio_gpu_driver); }
