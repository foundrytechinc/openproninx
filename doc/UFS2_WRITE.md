# UFS2 write-path work

The UFS2 reader is now accompanied by a writer. It can allocate direct blocks
and inodes, grow regular files within their direct-block limit, create and
remove directory entries, and maintain hard-link counts. A sudden power loss
can leave partially updated data or metadata; it does not yet provide
crash-safe UFS2 writes.

Before UFS2 may support general writable filesystem operations, PRONINX must
implement and test these operations:

1. Update cylinder-group and superblock summary counters together with free
   fragment and inode bitmaps.
2. Persist all required inode metadata, including timestamps, with validated
   on-disk offsets.
3. Define directory-entry coalescing and recovery for a failed namespace
   operation.
4. Define ordered metadata commits and test interruption recovery with
   `fsck_ffs` on images made by a supported FreeBSD release.

The root system image remains read-only until all four requirements are met.
This prevents a failed or interrupted write from silently corrupting a boot
volume.
