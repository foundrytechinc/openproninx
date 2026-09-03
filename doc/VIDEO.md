# Video and framebuffer console

OpenProninx has a kernel framebuffer console for the BIOS/QEMU boot path. It
is a text console rendered into a VBE linear framebuffer, not a general
graphics API or desktop environment.

`boot/video.S` calls BIOS VBE services before entering the 64-bit kernel and
selects the largest supported direct-colour RGB mode within
`VBE_MAX_WIDTH`/`VBE_MAX_HEIGHT` (default 1920x1080). Unsupported modes fall
back to VGA text mode. The boot stage publishes framebuffer dimensions, pitch,
pixel format, address, and the BIOS 8x16 font at physical address `0x900`.

`kern/framebuffer.c` validates and maps the framebuffer, clears it, and draws
the VGA palette and BIOS font with uniform scaling. `kern/console.c` keeps a
RAM shadow because VGA text memory cannot be read reliably after VBE is active.

The framebuffer is kernel-private: there is no `/dev/fb`, userspace pixel API,
window system, mouse support, or accelerated drawing API. Applications use the
normal console interface. QEMU's standard VGA device is the reference setup.
