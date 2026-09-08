# Video and Framebuffer Console

OpenProninx provides a high-performance kernel text console rendered into a linear framebuffer across both BIOS and UEFI boot paths, supporting multiple color depths, dynamic OpenBSD Spleen fonts, virtual terminals, and runtime mode switching.

## Boot & Display Architecture

1. **BIOS Boot Path:**
   - BIOS stage2 `video.c` uses VBE 2.0+ (`int 0x10`) and EDID queries to discover native panel resolutions.
   - `coreboot.c` provides direct framebuffer and hardware data extraction on coreboot, libreboot, and GNUboot payloads.
2. **UEFI Boot Path:**
   - `boot/uefi` initializes the UEFI Graphics Output Protocol (GOP), records the physical framebuffer aperture, dimensions, stride, and pixel format into `bootinfo`.
3. **Format & Aperture Support:**
   - 64-bit physical framebuffer aperture mapping via `ioremap_wc()` into kernel `IOMAP`.
   - Native rendering support for **15 bpp, 16 bpp (RGB565), 24 bpp (RGB888), and 32 bpp (ARGB8888)** color layouts.

---

## Virtual Terminals (VTs) & Device Nodes

- **4 Virtual Terminals:** The kernel maintains 4 independent virtual terminals (`struct vt`).
- **VT Switching:** Switch active terminals instantaneously using **`CTRL + ALT + F1`** through **`CTRL + ALT + F4`**. Scancodes `0x3b..0x3e` are translated in `kern/kbd.c` into `KBD_VT_BASE+n` (0x200), preventing any conflict with text characters.
- **Device Node Mapping:**
  - `/dev/tty1`, `/dev/tty2`, `/dev/tty3`, `/dev/tty4` are assigned to each specific virtual terminal.
  - `/dev/console` dynamically follows the currently active foreground terminal.

---

## Typography & Dynamic Spleen Fonts

OpenProninx imports four typeface sizes from the OpenBSD **Spleen** font family (`kern/font.c`, `kern/font.h`):
- **Spleen 5x8**, **8x16**, **12x24**, and **16x32**.
- The font size is dynamically selected at boot and mode switch based on screen width (matching OpenBSD `rasops` behavior) to preserve ideal terminal column counts (80–160 columns) across low-res VGA to 4K displays.

---

## High-Performance Rendering & Zero-Lag Architecture

### 1. Page Attribute Table (PAT) & Write-Combining (WC)
In `kern/vm.c`, the kernel configures `IA32_PAT` MSR (`0x277`) to map PA1 to Write-Combining (`0x01`). The framebuffer virtual memory region is mapped via `ioremap_wc()`. Writes to VRAM coalesce into 64-byte burst PCIe transactions.

### 2. RAM Shadow Buffer & Dirty Span Tracking
Physical video memory is strictly **write-only** to prevent hypervisor VM-Exit traps and bus wait states on read.
- `kern/framebuffer.c` maintains a primary RAM shadow buffer.
- Screen updates track dirty spans (bounding boxes of modified text cells), streaming only modified rectangular regions directly to VRAM in contiguous scanlines.

### 3. Runtime Video Mode Switching (Long Mode Real-Mode Thunk)
On BIOS systems, the kernel can switch video modes without rebooting:
- `kern/rmtramp.S`, `kern/realmode.c`, and `kern/vesa.c` implement a long-mode to 16-bit real-mode thunk (inspired by the 9front architecture).
- Userland can query available display modes via `ioctl(fd, FBIOGET_MODELIST, &modelist)` and inspect or switch modes using the `fbset` utility.

---

## Boundaries & Limitations
- The framebuffer is kernel-managed for text virtual terminals and console output.
- No userspace accelerated 3D graphics APIs (OpenGL/Vulkan) are present. Standard terminal I/O flows through standard Unix streams (`stdin`/`stdout`/`stderr`) and ANSI escape sequences.

