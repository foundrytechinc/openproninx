# Video and console

The console draws text into a linear framebuffer. There are four virtual
terminals, `/dev/tty1` to `tty4`. Each has its own cells, scrollback and
FreeBSD `teken` instance (xterm escape sequences, UTF-8, colours). Only the
front one is painted.

Pseudo terminals (`/dev/ttypN`, `/dev/ptypN`) and serial lines
(`/dev/ttyu0` to `ttyu3`) use the same line discipline, `kern/tty.c`,
written after OpenBSD's. Ptys are made eight at a time through `/dev/ptm`,
up to 992.

Kernel messages go to the first screen and the first serial line.
`/dev/console` is the serial line if the loader's `console` starts with
`comconsole` or there is no screen, otherwise the first screen. `/dev/tty`
is the session's terminal.

`stty` and `tail` are our own. `stty -g` prints settings it can read back.
`tail -f` reopens a file that was replaced.

## Framebuffer

- BIOS: `boot/video.c` reads the EDID and picks the VBE mode closest to the
  panel's native size, or 1024x768 if there is no EDID. `VBE_WIDTH` and
  `VBE_HEIGHT` force a size. Under QEMU the DISPI registers take any size.
- UEFI: `boot/uefi/video.c` does the same with GOP.
- coreboot: the framebuffer from the coreboot table.
- virtio-gpu: drawing goes to RAM and changed rectangles are flushed.

`kern/framebuffer.c` maps the framebuffer write-combining. Without one the
console uses VGA text mode at `0xb8000`.

## Fonts

`kern/font_spleen.c` has Spleen 2.2.0 in 8x16, 12x24, 16x32 and 32x64 with
Latin, Cyrillic, box drawing and punctuation. The size depends on screen
width, as in OpenBSD's `rasops_init`. `tools/fontcvt` builds the file from
the BDF fonts: `bmake font SPLEEN=<dir>`.

Characters missing from the font fall back to a close ASCII one, then `?`.

## Drawing

The screen is kept in RAM and video memory is only written, never read.
Only changed cells are redrawn and only changed scanline spans are copied.
A kernel thread repaints 25 times a second (`CONSOLE_REFRESH_HZ`), so heavy
output costs one repaint. Keyboard echo is immediate.

## Keyboard

The keyboard code is FreeBSD's `kbd(4)` and `atkbd(4)`. USB keyboards
(`kern/usb/ukbd.c`) produce the same scan codes and use the same tables,
with keys and LEDs found from the HID report descriptor.

`kern/kbd.c` holds the keymap: 256 keys, eight columns, AltGr, dead keys and
alt-keypad input. The default is the US map. All of FreeBSD's `vt` keymaps
are in `/usr/share/vt/keymaps/`.

Alt-F1 to Alt-F4 switch terminals. Ctrl-Alt-Delete goes to init. Left Alt
sends ESC first.

`kbdcontrol(1)` loads keymaps (`-l ru`), dumps them (`-d`, `-x`, `-L`), sets
the repeat rate (`-r`) and function keys (`-f`), and shows the keyboard
(`-i`). The keyboard ioctls only work on a console terminal. A pty or serial
line returns `FNU_ERR_BADREQUEST`.

Backspace sends DEL and Ctrl-H also erases (`VERASE2`).

## Mouse

`moused(8)` reads the mice and the console draws the pointer.

- Each mouse is `/dev/wsmouseN`. `ums` handles USB, `psm` PS/2.
- `moused` opens all of them, picks up new ones from `/dev/hotplug`, and
  handles acceleration, button mapping, three-button emulation, the wheel and
  trackpoint scrolling. It writes to `/dev/consolectl`.
- `/dev/sysmouse` gives the events in MouseSystems format for programs.
- Left button selects (double click for a word, triple for a line), right
  extends, middle pastes. Programs that send `ESC [ ? 1000 h` get xterm
  mouse reports. `vidcontrol -m on|off` shows or hides the pointer.

## PS/2

`kern/psm.c` and `kern/atkbdc.c` follow FreeBSD's `psm(4)` and `atkbdc(4)`:
the same probe order, sync checking, Synaptics and Elantech touchpads (taps,
scrolling, palm detection, clickpads) and the TrackPoint behind them. The
`hw.psm.*` sysctls use FreeBSD's names. `/dev/psm0` and `/dev/bpsm0` give
the raw device. There is no evdev.

## Video modes

`vidcontrol -i mode` lists modes, `-i active` shows the current one, and
`vidcontrol <width>x<height>` switches. These ioctls and `CONSMOUSE` only
work on a console terminal, so an ssh session cannot change the screen.

- QEMU: any size that fits.
- BIOS: modes from the video BIOS, called in real mode through
  `kern/realmode.c`.
- UEFI: only the mode the firmware set.

After S3 under UEFI the firmware does not restore the display. The UEFI
loader keeps the PCI option ROMs, and the kernel runs the display ROM in
x86emu to set the mode again. Without a ROM the screen stays dark after S3,
so `auto` uses idle sleep there (see `doc/ACPI.md`).

## Other

- The framebuffer belongs to the kernel. There is no `/dev/fb`.
- Scroll Lock enters the scrollback (1000 lines). Page Up/Down, arrows,
  Home and End move, Scroll Lock leaves.
- `kern.vt.kbd_halt`, `kbd_poweroff`, `kbd_reboot` and `kbd_panic` enable
  or disable the key combinations.
- `TIOCGWINSZ` returns the size in cells and pixels.
