#include "inc/framebuffer.h"

/*
 * This code is loaded with the rest of the boot block but is called while
 * the CPU is still in real mode.  It chooses the largest 32-bit linear VBE
 * mode not exceeding the build-time VBE_MAX_WIDTH/VBE_MAX_HEIGHT limits.  If
 * VBE is unavailable, the existing 80x25
 * text mode remains active.
 */
.section .text
.code16
.globl vbe_setup
vbe_setup:
  pushw %ds
  pushw %es
  pusha

  xorw %ax, %ax
  movw %ax, %ds
  # Do not let framebuffer data left by a warm reboot look valid when VBE
  # probing or mode setup fails.
  movl %eax, BOOT_FRAMEBUFFER_INFO_PHYS
  movl $0x32454256, 0x500       # "VBE2": request the extended info block
  xorw %ax, %ax
  movw %ax, %es
  movw $0x4f00, %ax
  movw $0x500, %di
  int $0x10
  cmpw $0x004f, %ax
  je vbe_info_ok
  movl $0x46423030, BOOT_FRAMEBUFFER_INFO_PHYS # "FB00": VBE info failed
  jmp vbe_done

vbe_info_ok:

  movw $0xffff, 0x8f0           # selected mode
  movl $0, 0x8f2                # selected pixel count
  # VideoModePtr is a far pointer at offsets 0x0e (offset), 0x10 (segment).
  # Do not use the adjacent Capabilities field as a pointer.
  movw 0x510, %ax               # VideoModePtr.segment
  movw %ax, %es
  movw 0x50e, %si               # VideoModePtr.offset

vbe_next_mode:
  movw %es:(%si), %bx
  addw $2, %si
  cmpw $0xffff, %bx
  je vbe_set_mode

  # Only AX is specified as an output from VBE.  Preserve the iterator and
  # the candidate mode across BIOS implementations that clobber registers.
  pushw %es
  pushw %si
  pushw %bx
  xorw %ax, %ax
  movw %ax, %es
  movw %bx, %cx
  movw $0x4f01, %ax
  movw $0x700, %di
  int $0x10
  popw %bx
  popw %si
  popw %es
  cmpw $0x004f, %ax
  jne vbe_next_mode

  movw 0x700, %ax               # mode attributes
  andw $0x0091, %ax             # supported, graphics, linear framebuffer
  cmpw $0x0091, %ax
  jne vbe_next_mode
  cmpb $24, 0x719
  je vbe_bpp_supported
  cmpb $32, 0x719
  jne vbe_next_mode
vbe_bpp_supported:
  cmpb $6, 0x71b                # direct-color pixel format
  jne vbe_next_mode
  # Direct-colour masks start at offset 0x1f in the VBE ModeInfoBlock.
  # Select the B8G8R8(/X8) layout that framebuffer.c writes.
  cmpb $8, 0x71f                # RedMaskSize
  jne vbe_next_mode
  cmpb $16, 0x720               # RedFieldPosition
  jne vbe_next_mode
  cmpb $8, 0x721                # GreenMaskSize
  jne vbe_next_mode
  cmpb $8, 0x722                # GreenFieldPosition
  jne vbe_next_mode
  cmpb $8, 0x723                # BlueMaskSize
  jne vbe_next_mode
  cmpb $0, 0x724                # BlueFieldPosition
  jne vbe_next_mode
  movw 0x712, %ax               # width
  cmpw $VBE_MAX_WIDTH, %ax
  ja vbe_next_mode
  movw 0x714, %cx               # height
  cmpw $VBE_MAX_HEIGHT, %cx
  ja vbe_next_mode

  movzwl %ax, %eax
  movzwl %cx, %ecx
  imull %ecx, %eax
  cmpl 0x8f2, %eax
  jbe vbe_next_mode
  movl %eax, 0x8f2
  movw %bx, 0x8f0
  jmp vbe_next_mode

vbe_set_mode:
  cmpw $0xffff, 0x8f0
  jne vbe_have_mode
  # Bochs/QEMU's standard VBE extension always provides this RGB mode.
  # Keep it as a fallback for firmware with a malformed mode list.
  movw $VBE_FALLBACK_MODE, 0x8f0
vbe_have_mode:
  movw 0x8f0, %bx
  orw $0x4000, %bx              # request linear framebuffer
  movw $0x4f02, %ax
  int $0x10
  cmpw $0x004f, %ax
  je vbe_mode_set
  movl $0x46423032, BOOT_FRAMEBUFFER_INFO_PHYS # "FB02": set mode failed
  jmp vbe_done

vbe_mode_set:
  # Query the selected mode once more and publish it for the 64-bit kernel.
  movw 0x8f0, %cx
  xorw %ax, %ax
  movw %ax, %es
  movw $0x4f01, %ax
  movw $0x700, %di
  int $0x10
  cmpw $0x004f, %ax
  je vbe_mode_info
  movl $0x46423031, BOOT_FRAMEBUFFER_INFO_PHYS # "FB01": mode info failed
  jmp vbe_done

vbe_mode_info:
  movl $BOOT_FRAMEBUFFER_MAGIC, BOOT_FRAMEBUFFER_INFO_PHYS
  # PhysBasePtr is at offset 0x28.
  movl 0x728, %eax
  movl %eax, BOOT_FRAMEBUFFER_INFO_PHYS + 4
  movw 0x712, %ax
  movw %ax, BOOT_FRAMEBUFFER_INFO_PHYS + 8
  movw 0x714, %ax
  movw %ax, BOOT_FRAMEBUFFER_INFO_PHYS + 10
  movw 0x710, %ax
  movw %ax, BOOT_FRAMEBUFFER_INFO_PHYS + 12
  movb 0x719, %al
  movb %al, BOOT_FRAMEBUFFER_INFO_PHYS + 14

  # BIOS ROM supplies the standard 8x16 VGA font.  The renderer scales it.
  movw $0x1130, %ax
  movb $0x06, %bh
  int $0x10
  movw %es, %ax
  movzwl %ax, %eax
  shll $4, %eax
  movzwl %bp, %edx
  addl %edx, %eax
  movl %eax, BOOT_FRAMEBUFFER_INFO_PHYS + 16

vbe_done:
  popa
  popw %es
  popw %ds
  ret

.section .note.GNU-stack,"",@progbits
