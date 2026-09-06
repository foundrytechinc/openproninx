#ifndef FS_IMG_PATH
#define FS_IMG_PATH "obj/fs.img"
#endif

.section .data
.p2align 12
.globl ramdisk_blob_start
.globl ramdisk_blob_end
ramdisk_blob_start:
  .incbin FS_IMG_PATH
ramdisk_blob_end:

.section .note.GNU-stack,"",@progbits
