# ACPI

The ACPI code is in `kern/acpi/`. It reads the firmware's tables, runs the
AML in them and handles the events the machine sends. SMBIOS is read by
`kern/smbios.c` and the sensors are in `kern/sensors.c`.

## Boot

1. `acpi_init` (`tables.c`) finds the RSDP in `bootinfo` or in the BIOS areas
   and keeps the FADT, MADT, FACS and DSDT. The MADT gives the processors
   and I/O APICs.
2. `smbios_init` sets `hw.vendor`, `hw.product` and `hw.version`. The serial
   number and UUID are readable by root only.
3. `acpi_start` (`ospm.c`) loads the DSDT and the SSDTs, builds the GPE
   table, switches to ACPI mode, calls `\_PIC(1)` and `_INI`, reads the PCI
   routing and attaches the devices.
4. The `acpi` thread runs on the boot processor. It handles SCI events one at
   a time and polls every ten seconds.

## Interpreter

`aml.h` is the interface. Values are reference counted and the namespace is a
tree of `aml_node`. `aml_parse.c` holds the opcode table and the evaluator,
`aml_field.c` fields, mutexes, events and the global lock. Operation regions
for memory, I/O and PCI config are in `gas.c`; the EC driver registers its
own. One recursive lock (`aml_lock`) serialises evaluation. An AML error
stops that evaluation and the message names the method and offset.

`_OSI` answers yes to the Windows versions, and to Darwin on Apple hardware.

## Drivers

`dev.c` matches `_HID` and `_CID` against the drivers. The EC attaches first.

| file | devices | what it does |
| --- | --- | --- |
| `ec.c` | PNP0C09, ECDT | EC region, `_Qxx` events |
| `button.c` | PNP0C0C, PNP0C0E, PNP0C0D | power and sleep buttons, lid |
| `ac.c` | ACPI0003 | `hw.acpi.acline` |
| `battery.c` | PNP0C0A | `hw.acpi.battery.*`, `hw.sensors.acpibatN` |
| `thermal.c` | thermal zones | `hw.sensors.acpitzN.temp0`, fans, passive cooling, `_CRT` power off |
| `prt.c` | `_PRT` | PCI interrupt routing |
| `tpm.c` | MSFT0101, PNP0C31 | TPM 1.2 and 2.0, see below |
| `cpu.c` | processors | P-states |
| `video.c` | `_BCM`, `_BCL` | panel backlight for idle sleep |

The buttons and the lid follow FreeBSD's sysctls:
`hw.acpi.power_button_state` (default `S5`), `hw.acpi.sleep_button_state`
(`S3`) and `hw.acpi.lid_switch_state` (`S3`). `NONE` makes the kernel ignore
it. Power off goes through init.

## TPM

The TPM gets `TPM2_Shutdown(STATE)` before S3 or S4, `TPM2_Shutdown(CLEAR)`
before power off, and `TPM_SaveState` on a 1.2. It is reached through CRB or
the TIS FIFO, whichever the TPM2 table says.

It adds 32 random bytes to the entropy pool at attach and 16 every ten
seconds. `kern.random.harvest.tpm=0` turns that off and
`kern.random.harvest.tpm_bytes` counts the bytes.

After a power cut a TPM can answer with leftovers of an old command. An
answer is used only if it has the right tag and size and is not our own
command echoed back. Otherwise the interface is reset (cancel, goIdle and
relinquish on CRB, commandReady on TIS) and the command is sent once more.
`kern.random.harvest.tpm_garbage` counts these.

`/dev/tpm0` is root only and one process at a time. Write a whole command,
read the answer.

## Processor speed

`powerd(8)` picks the speed and the kernel sets it. `hw.cpu.levels` lists the
`_PSS` speeds in MHz and `hw.cpu.nominal` is the fastest one without turbo.
Writing `hw.cpu.freq` selects the fastest state at or below the value,
limited by `_PPC` and the thermal zones. `hw.cpuspeed` is the current speed
with turbo.

`powerd` has FreeBSD's modes and options, plus `-A` and `-B` to cap the speed
on AC and on battery. The service runs with `-B nominal`. Change it in
`/etc/rc.d/powerd`.

## Sleep

`zzz`, the sleep button and the lid all ask for a sleep to RAM.
`hw.acpi.suspend_state` decides which one:

- `auto` (default) uses S3 when something can bring the screen back, and idle
  sleep otherwise. The boot line `VIDEO: ... back from sleep ...` tells
  which. A coreboot with libgfxinit does not set up the display on resume, so
  without a video BIOS the screen would stay dark.
- `idle` stops the processes, flushes the buffers, holds the disks and turns
  the backlight down with `_BCM`. The firmware is not involved. A key, the
  power or sleep button or opening the lid wakes it.
- `S1` keeps the devices powered. Some firmware turns things off in `_PTS`
  and never back on (on the T520 that is USB), so it is never picked by
  `auto`.
- `S3` always uses S3.

An idle processor waits in the deepest MWAIT state CPUID offers if the APIC
timer and the TSC keep running there. Otherwise it uses `hlt`. `hw.cpu.idle`
shows which.

The S3 path in `sleep.c`: `_TTS`, buffers to disk, USB buses and drivers
suspended, PCI config saved, other processors parked, `_PTS`, wake devices
armed, local APIC masked, wake code (`wakecode.S`) copied below 1 MB, then
SLP_TYP and SLP_EN. On wake the same in reverse. After a BIOS boot the video
BIOS is run again in x86emu. `hw.acpi.reset_video=1` runs it in real mode
instead (this hangs the Y520).

Every USB controller is reset after a sleep. Mass storage comes back if the
same device, by port, ids and serial number, shows up within 2 seconds.

`debug.pm_trace=1` records each step of the sleep and wake in the RTC date.
After a hang, the next boot prints where it stopped. The clock must be set
again afterwards.

## Hibernate

`hibernate`, `sysctl hw.acpi.sleep=4` or a button set to `S4`.
`hw.acpi.hibernate_mode` is `shutdown` (default) or `platform`. `shutdown`
writes the image and powers off through `\_S5`. `platform` also calls
`_PTS(4)` and enters `\_S4`. Do not use `platform` on the T520: its `_PTS`
cuts USB power before the image is written.

The image format follows OpenBSD's `subr_hibernate.c` and the writing follows
Linux swsusp, so it works on any disk the block layer has.

1. A 16 MB piglet is reserved at boot. Memory for the image is set aside
   after the processes are frozen.
2. With interrupts off, memory in use is compressed with LZ4 in 4 MB chunks.
3. Devices come back, the image goes to the longest free run in `fnu-swap`,
   then the chunk table and the signature. The machine powers off.
4. If the image does not fit, memory is paged out and it starts again.
5. On the next boot of the same kernel the signature is found before the
   root is mounted and cleared. The image is unpacked over the running kernel
   from page tables in the piglet, then the wake continues as from S3.

`fnu-swap` must be bigger than the memory in use. `SWAP_MB` sets its size.

## Power off and reset

`acpi_poweroff_prepare` calls `_PTS(5)` and `\_SI._SST(0)` and disables the
GPEs. `acpi_poweroff` then writes the sleep type and enable bit. The FADT
reset register is one of the reset methods in `kern/shutdown.c`.

## Missing

PCI segments other than 0.

## Testing

The interpreter was run on the host over the DSDT and SSDTs of the T520, the
Y520 and a desktop. In QEMU a test SSDT provides AC, batteries, a lid, a
button and a thermal zone. S4 was tested in QEMU on IDE, AHCI, NVMe and USB,
and `tests/hibtest` checks memory across it. S3, S4 and idle sleep were tested
on the T520 and the Y520.
