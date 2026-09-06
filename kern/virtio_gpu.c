#include "virtio_gpu.h"
#include "defs.h"
#include "driver.h"
#include "memlayout.h"
#include "pci.h"
#include "spinlock.h"
#include "x86.h"

#define VIRTQ_SIZE 16

static struct spinlock gpu_lock;
static int gpu_active = 0;
static uchar *gpu_pixels = 0;
static uint64_t gpu_phys = 0;
static uint gpu_width = 1024;
static uint gpu_height = 768;
static uint gpu_pitch = 1024 * 4;

static volatile struct virtio_pci_common_cfg *gpu_common = 0;
static volatile uint16_t *gpu_notify_reg = 0;
static volatile uint8_t *gpu_isr_reg = 0;

static struct vring_desc controlq_desc[VIRTQ_SIZE] __attribute__((aligned(16)));
static struct vring_avail controlq_avail __attribute__((aligned(2)));
static struct vring_used controlq_used __attribute__((aligned(4)));
static uint16_t last_used_idx = 0;

int virtio_gpu_available(void) {
  return gpu_active;
}

void *virtio_gpu_framebuffer(void) {
  return (void *)gpu_pixels;
}

uint virtio_gpu_width(void) {
  return gpu_width;
}

uint virtio_gpu_height(void) {
  return gpu_height;
}

uint virtio_gpu_pitch(void) {
  return gpu_pitch;
}

static int virtio_gpu_exec(void *cmd, uint32_t cmd_sz, void *resp, uint32_t resp_sz) {
  acquire(&gpu_lock);

  // Setup descriptor 0: request buffer
  controlq_desc[0].address = V2P(cmd);
  controlq_desc[0].length = cmd_sz;
  controlq_desc[0].flags = 1; // VRING_DESC_F_NEXT
  controlq_desc[0].next = 1;

  // Setup descriptor 1: response buffer
  controlq_desc[1].address = V2P(resp);
  controlq_desc[1].length = resp_sz;
  controlq_desc[1].flags = 2; // VRING_DESC_F_WRITE
  controlq_desc[1].next = 0;

  // Put on available ring
  uint16_t avail_idx = controlq_avail.index;
  controlq_avail.ring[avail_idx % VIRTQ_SIZE] = 0;
  __sync_synchronize();
  controlq_avail.index = avail_idx + 1;
  __sync_synchronize();

  // Notify host
  *gpu_notify_reg = 0;

  // Synchronous poll with timeout
  int timeout = 100000;
  while (controlq_used.index == last_used_idx && --timeout > 0) {
    microdelay(10);
  }
  if (timeout == 0) {
    release(&gpu_lock);
    cprintf("VIRTIO-GPU: command timeout\n");
    return -1;
  }
  last_used_idx = controlq_used.index;

  release(&gpu_lock);
  return 0;
}

void virtio_gpu_flush_rect(uint x, uint y, uint w, uint h) {
  if (!gpu_active || w == 0 || h == 0)
    return;

  if (x + w > gpu_width)
    w = gpu_width - x;
  if (y + h > gpu_height)
    h = gpu_height - y;

  struct virtio_gpu_transfer_to_host_2d cmd_xfer;
  struct virtio_gpu_resource_flush cmd_flush;
  struct virtio_gpu_ctrl_hdr resp;

  // 1. Transfer dirty rect from guest backing RAM to host GPU resource
  memset(&cmd_xfer, 0, sizeof(cmd_xfer));
  cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  cmd_xfer.resource_id = 1;
  cmd_xfer.offset = ((uint64_t)y * gpu_pitch) + (x * 4);
  cmd_xfer.rect.x = x;
  cmd_xfer.rect.y = y;
  cmd_xfer.rect.width = w;
  cmd_xfer.rect.height = h;
  virtio_gpu_exec(&cmd_xfer, sizeof(cmd_xfer), &resp, sizeof(resp));

  // 2. Flush host GPU resource to scanout (screen)
  memset(&cmd_flush, 0, sizeof(cmd_flush));
  cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  cmd_flush.resource_id = 1;
  cmd_flush.rect.x = x;
  cmd_flush.rect.y = y;
  cmd_flush.rect.width = w;
  cmd_flush.rect.height = h;
  virtio_gpu_exec(&cmd_flush, sizeof(cmd_flush), &resp, sizeof(resp));
}

void virtio_gpu_flush_all(void) {
  virtio_gpu_flush_rect(0, 0, gpu_width, gpu_height);
}

static int virtio_gpu_probe(struct device *dev) {
  if (dev->pci.vendor_id == 0x1af4 && dev->pci.device_id == 0x1050)
    return 10;
  return 0;
}

static int virtio_gpu_attach(struct device *dev) {
  pci_enable_bus_master(&dev->pci);

  // Discover VirtIO PCI capabilities
  uint8_t cap_ptr = pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function, 0x34);
  uint8_t common_bar = 0xff, notify_bar = 0xff, isr_bar = 0xff;
  uint32_t common_off = 0, notify_off = 0, notify_mult = 0, isr_off = 0;

  while (cap_ptr) {
    uint8_t cap_id = pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr);
    if (cap_id == 0x09) {
      uint8_t cfg_type = pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr + 3);
      uint8_t bar = pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr + 4);
      uint32_t off = pci_read32(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr + 8);
      if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
        common_bar = bar;
        common_off = off;
      } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
        notify_bar = bar;
        notify_off = off;
        notify_mult = pci_read32(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr + 16);
      } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
        isr_bar = bar;
        isr_off = off;
      }
    }
    cap_ptr = pci_read8(dev->pci.bus, dev->pci.device, dev->pci.function, cap_ptr + 1);
  }

  if (common_bar >= 6 || notify_bar >= 6 || dev->pci.bar[common_bar] == 0) {
    cprintf("VIRTIO-GPU: required PCI capabilities missing\n");
    return -1;
  }

  uint8_t *common_base = (uint8_t *)DEVSPACE_P2V(dev->pci.bar[common_bar]) + common_off;
  uint8_t *notify_base = (uint8_t *)DEVSPACE_P2V(dev->pci.bar[notify_bar]) + notify_off;

  gpu_common = (volatile struct virtio_pci_common_cfg *)common_base;
  if (isr_bar < 6 && dev->pci.bar[isr_bar]) gpu_isr_reg = (volatile uint8_t *)DEVSPACE_P2V(dev->pci.bar[isr_bar]) + isr_off;

  // Reset device
  gpu_common->device_status = 0;
  __sync_synchronize();
  gpu_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
  __sync_synchronize();

  // Accept modern VirtIO 1.0 feature
  gpu_common->device_feature_select = 1;
  uint32_t f1 = gpu_common->device_feature;
  gpu_common->driver_feature_select = 1;
  gpu_common->driver_feature = f1 & (1U << (VIRTIO_F_VERSION_1 - 32));
  gpu_common->device_feature_select = 0;
  gpu_common->driver_feature_select = 0;
  gpu_common->driver_feature = 0;
  __sync_synchronize();

  gpu_common->device_status |= VIRTIO_STATUS_FEATURES_OK;
  __sync_synchronize();
  if (!(gpu_common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
    cprintf("VIRTIO-GPU: FEATURES_OK rejected\n");
    return -1;
  }

  // Configure controlq (queue 0)
  gpu_common->queue_select = 0;
  uint16_t qsize = gpu_common->queue_size;
  if (qsize < VIRTQ_SIZE) {
    cprintf("VIRTIO-GPU: queue size too small (%d)\n", qsize);
    return -1;
  }
  gpu_common->queue_size = VIRTQ_SIZE;
  gpu_common->queue_desc = V2P(controlq_desc);
  gpu_common->queue_driver = V2P(&controlq_avail);
  gpu_common->queue_device = V2P(&controlq_used);
  gpu_common->queue_enable = 1;
  __sync_synchronize();

  uint16_t q0_notify_off = gpu_common->queue_notify_off;
  gpu_notify_reg = (volatile uint16_t *)(notify_base + q0_notify_off * notify_mult);

  gpu_common->device_status |= VIRTIO_STATUS_DRIVER_OK;
  __sync_synchronize();

  // 1. Discover host display resolution
  struct virtio_gpu_ctrl_hdr cmd_info;
  struct virtio_gpu_resp_display_info resp_info;
  memset(&cmd_info, 0, sizeof(cmd_info));
  cmd_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
  memset(&resp_info, 0, sizeof(resp_info));

  uint probed_w = 0, probed_h = 0;
  if (virtio_gpu_exec(&cmd_info, sizeof(cmd_info), &resp_info, sizeof(resp_info)) == 0 &&
      resp_info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
    for (int s = 0; s < VIRTIO_GPU_MAX_SCANOUTS; s++) {
      if (resp_info.pmodes[s].enabled &&
          resp_info.pmodes[s].rect.width > 0 &&
          resp_info.pmodes[s].rect.height > 0) {
        probed_w = resp_info.pmodes[s].rect.width;
        probed_h = resp_info.pmodes[s].rect.height;
        break;
      }
    }
  }

  if (probed_w >= 1024 && probed_w <= 3840 && probed_h >= 768 && probed_h <= 2160) {
    gpu_width = probed_w;
    gpu_height = probed_h;
  } else {
    gpu_width = 1024;
    gpu_height = 768;
  }
  gpu_pitch = gpu_width * 4;
  uint32_t fb_bytes = gpu_pitch * gpu_height;

  // Allocate contiguous backing buffer
  gpu_pixels = (uchar *)gpu_alloc_backing(fb_bytes, &gpu_phys);
  if (!gpu_pixels || !gpu_phys) {
    cprintf("VIRTIO-GPU: failed to allocate framebuffer backing memory (%d bytes)\n", fb_bytes);
    return -1;
  }

  struct virtio_gpu_ctrl_hdr resp;

  // 2. Create 2D host resource
  struct virtio_gpu_resource_create_2d cmd_create;
  memset(&cmd_create, 0, sizeof(cmd_create));
  cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  cmd_create.resource_id = 1;
  cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  cmd_create.width = gpu_width;
  cmd_create.height = gpu_height;
  if (virtio_gpu_exec(&cmd_create, sizeof(cmd_create), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    cprintf("VIRTIO-GPU: RESOURCE_CREATE_2D failed (resp 0x%x)\n", resp.type);
    return -1;
  }

  // 3. Attach backing memory
  struct virtio_gpu_resource_attach_backing cmd_attach;
  memset(&cmd_attach, 0, sizeof(cmd_attach));
  cmd_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  cmd_attach.resource_id = 1;
  cmd_attach.nr_entries = 1;
  cmd_attach.entries[0].addr = gpu_phys;
  cmd_attach.entries[0].length = (fb_bytes + 4095) & ~4095U;
  if (virtio_gpu_exec(&cmd_attach, sizeof(cmd_attach), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    cprintf("VIRTIO-GPU: RESOURCE_ATTACH_BACKING failed (resp 0x%x)\n", resp.type);
    return -1;
  }

  // 4. Connect scanout 0 to resource 1
  struct virtio_gpu_set_scanout cmd_scanout;
  memset(&cmd_scanout, 0, sizeof(cmd_scanout));
  cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  cmd_scanout.scanout_id = 0;
  cmd_scanout.resource_id = 1;
  cmd_scanout.rect.x = 0;
  cmd_scanout.rect.y = 0;
  cmd_scanout.rect.width = gpu_width;
  cmd_scanout.rect.height = gpu_height;
  if (virtio_gpu_exec(&cmd_scanout, sizeof(cmd_scanout), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    cprintf("VIRTIO-GPU: SET_SCANOUT failed (resp 0x%x)\n", resp.type);
    return -1;
  }

  // Clear memory and flush scanout
  memset(gpu_pixels, 0, gpu_pitch * gpu_height);
  gpu_active = 1;
  virtio_gpu_flush_all();

  // Initialize framebuffer and switch console
  framebuffer_init_gpu(gpu_pixels, gpu_width, gpu_height, gpu_pitch);
  console_switch_to_gpu();

  cprintf("VIRTIO-GPU: display activated (%dx%d 32bpp accelerated)\n", gpu_width, gpu_height);
  return 0;
}

int virtio_gpu_set_resolution(uint w, uint h) {
  if (!gpu_active)
    return -1;

  if (w < 320 || w > 3840 || h < 200 || h > 2160)
    return -1;

  uint32_t new_pitch = w * 4;
  uint32_t new_fb_bytes = new_pitch * h;
  uint64_t phys = 0;
  if (!gpu_alloc_backing(new_fb_bytes, &phys))
    return -1;

  struct virtio_gpu_ctrl_hdr resp;

  // 1. Detach old backing & unref old resource
  struct virtio_gpu_resource_detach_backing cmd_detach;
  memset(&cmd_detach, 0, sizeof(cmd_detach));
  cmd_detach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
  cmd_detach.resource_id = 1;
  virtio_gpu_exec(&cmd_detach, sizeof(cmd_detach), &resp, sizeof(resp));

  struct virtio_gpu_resource_unref cmd_unref;
  memset(&cmd_unref, 0, sizeof(cmd_unref));
  cmd_unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
  cmd_unref.resource_id = 1;
  virtio_gpu_exec(&cmd_unref, sizeof(cmd_unref), &resp, sizeof(resp));

  // 2. Create 2D host resource
  struct virtio_gpu_resource_create_2d cmd_create;
  memset(&cmd_create, 0, sizeof(cmd_create));
  cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  cmd_create.resource_id = 1;
  cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  cmd_create.width = w;
  cmd_create.height = h;
  if (virtio_gpu_exec(&cmd_create, sizeof(cmd_create), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    return -1;
  }

  // 3. Attach backing memory
  struct virtio_gpu_resource_attach_backing cmd_attach;
  memset(&cmd_attach, 0, sizeof(cmd_attach));
  cmd_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  cmd_attach.resource_id = 1;
  cmd_attach.nr_entries = 1;
  cmd_attach.entries[0].addr = gpu_phys;
  cmd_attach.entries[0].length = (new_fb_bytes + 4095) & ~4095U;
  if (virtio_gpu_exec(&cmd_attach, sizeof(cmd_attach), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    return -1;
  }

  // 4. Connect scanout 0 to resource 1
  struct virtio_gpu_set_scanout cmd_scanout;
  memset(&cmd_scanout, 0, sizeof(cmd_scanout));
  cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  cmd_scanout.scanout_id = 0;
  cmd_scanout.resource_id = 1;
  cmd_scanout.rect.x = 0;
  cmd_scanout.rect.y = 0;
  cmd_scanout.rect.width = w;
  cmd_scanout.rect.height = h;
  if (virtio_gpu_exec(&cmd_scanout, sizeof(cmd_scanout), &resp, sizeof(resp)) < 0 ||
      resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
    return -1;
  }

  gpu_width = w;
  gpu_height = h;
  gpu_pitch = new_pitch;

  memset(gpu_pixels, 0, gpu_pitch * gpu_height);
  virtio_gpu_flush_all();

  framebuffer_init_gpu(gpu_pixels, gpu_width, gpu_height, gpu_pitch);
  console_switch_to_gpu();

  cprintf("VIRTIO-GPU: resolution changed to %dx%d (32bpp)\n", gpu_width, gpu_height);
  return 0;
}

static void virtio_gpu_intr(struct device *dev) {
  if (gpu_isr_reg) {
    volatile uint8_t isr = *gpu_isr_reg;
    (void)isr;
  }
}

static struct driver virtio_gpu_driver = {
  .name = "virtio_gpu",
  .bus_type = BUS_TYPE_PCI,
  .dev_type = DEVICE_TYPE_DISPLAY,
  .probe = virtio_gpu_probe,
  .attach = virtio_gpu_attach,
  .intr = virtio_gpu_intr,
};

int virtio_gpu_driver_init(void) {
  initlock(&gpu_lock, "virtio_gpu");
  return driver_register(&virtio_gpu_driver);
}
