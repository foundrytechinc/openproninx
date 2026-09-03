#include "inc/types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "inc/stat.h"
#include "fs.h"
#include "file.h"
#include "buf.h"
#include "fat32.h"

static uint cluster_to_sector(uint cluster);
static void set_next_cluster(int dev, uint cluster, uint next);
static uint alloc_cluster(int dev);
static uint next_cluster(int dev, uint cluster);
static int is_eof(uint cluster);
static void generate_83_name(char *name, char *res);
static void free_cluster_chain(int dev, uint cluster);

static struct {
    struct fat32_bpb bpb;
    uint fat_start_sector;
    uint data_start_sector;
    uint sectors_per_cluster;
    int valid;
} fat_vol;

static uint cluster_to_sector(uint cluster) {
    return fat_vol.data_start_sector + (cluster - 2) * fat_vol.sectors_per_cluster;
}

static void set_next_cluster(int dev, uint cluster, uint next) {
    uint fat_sector = fat_vol.fat_start_sector + (cluster * 4) / BSIZE;
    uint fat_offset = (cluster * 4) % BSIZE;
    struct buf *bp = bread(dev, fat_sector);
    *(uint*)(bp->data + fat_offset) = next & 0x0FFFFFFF;
    log_write(bp);
    brelse(bp);
}

static uint alloc_cluster(int dev) {
    uint total_clusters = fat_vol.bpb.total_sectors_32 / fat_vol.sectors_per_cluster;
    for (uint c = 2; c < total_clusters; c++) {
        uint fat_sector = fat_vol.fat_start_sector + (c * 4) / BSIZE;
        uint fat_offset = (c * 4) % BSIZE;
        struct buf *bp = bread(dev, fat_sector);
        uint val = *(uint*)(bp->data + fat_offset) & 0x0FFFFFFF;
        if (val == 0) {
            *(uint*)(bp->data + fat_offset) = 0x0FFFFFFF; // Mark as EOF
            log_write(bp);
            brelse(bp);
            
            // Zero out the cluster
            uint sector = cluster_to_sector(c);
            for (uint i = 0; i < fat_vol.sectors_per_cluster; i++) {
                struct buf *dbp = bread(dev, sector + i);
                memset(dbp->data, 0, BSIZE);
                log_write(dbp);
                brelse(dbp);
            }
            return c;
        }
        brelse(bp);
    }
    panic("alloc_cluster: out of clusters");
}

static uint next_cluster(int dev, uint cluster) {
    uint fat_sector = fat_vol.fat_start_sector + (cluster * 4) / BSIZE;
    uint fat_offset = (cluster * 4) % BSIZE;
    struct buf *bp = bread(dev, fat_sector);
    uint next = *(uint*)(bp->data + fat_offset);
    brelse(bp);
    return next & 0x0FFFFFFF;
}

static int is_eof(uint cluster) {
    return cluster >= 0x0FFFFFF8;
}

void fat32_init(int dev) {
    struct buf *bp = bread(dev, 0);
    struct fat32_bpb *bpb = (struct fat32_bpb*)bp->data;
    
    if (bpb->boot_signature != 0x28 && bpb->boot_signature != 0x29) {
        brelse(bp);
        return;
    }

    memmove(&fat_vol.bpb, bp->data, sizeof(struct fat32_bpb));
    brelse(bp);

    fat_vol.fat_start_sector = fat_vol.bpb.reserved_sectors;
    fat_vol.data_start_sector = fat_vol.bpb.reserved_sectors + (fat_vol.bpb.fat_count * fat_vol.bpb.fat_size_32);
    fat_vol.sectors_per_cluster = fat_vol.bpb.sectors_per_cluster;
    fat_vol.valid = 1;
    
    cprintf("FAT32: Initialized volume. Data start: %d, Root cluster: %d, Sectors/Cluster: %d\n", 
            fat_vol.data_start_sector, fat_vol.bpb.root_cluster, fat_vol.sectors_per_cluster);
}

uint fat32_root_cluster(void) {
    return fat_vol.bpb.root_cluster;
}

int fat32_read_cluster(int dev, uint cluster, char *buf, uint n) {
    if (!fat_vol.valid) return -1;
    
    uint sector = cluster_to_sector(cluster);
    for (uint i = 0; i < fat_vol.sectors_per_cluster && n > 0; i++) {
        struct buf *bp = bread(dev, sector + i);
        uint to_read = (n > BSIZE) ? BSIZE : n;
        memmove(buf, bp->data, to_read);
        brelse(bp);
        buf += to_read;
        n -= to_read;
    }
    return 0;
}

uint fat32_lookup(int dev, uint dir_cluster, char *name, struct fat32_dirent *res, uint *off) {
    char fatname[11];
    generate_83_name(name, fatname);
    
    uint current_cluster = dir_cluster;
    uint cluster_count = 0;
    
    while (!is_eof(current_cluster)) {
        uint sector = cluster_to_sector(current_cluster);
        for (uint i = 0; i < fat_vol.sectors_per_cluster; i++) {
            struct buf *bp = bread(dev, sector + i);
            struct fat32_dirent *de = (struct fat32_dirent*)bp->data;
            for (uint j = 0; j < BSIZE / sizeof(struct fat32_dirent); j++) {
                uint total_off = cluster_count * fat_vol.sectors_per_cluster * BSIZE + i * BSIZE + j * sizeof(struct fat32_dirent);
                
                if (de[j].name[0] == 0) {
                    if (off) *off = total_off;
                    brelse(bp);
                    return 0;
                }
                if (de[j].name[0] == 0xE5) {
                    continue;
                }
                if (de[j].attr == FAT32_ATTR_LONG_NAME) {
                    continue;
                }
                
                if (memcmp(de[j].name, fatname, 11) == 0) {
                    if (res) memmove(res, &de[j], sizeof(struct fat32_dirent));
                    if (off) *off = total_off;
                    uint cluster = ((uint)de[j].first_cluster_high << 16) | de[j].first_cluster_low;
                    brelse(bp);
                    return cluster;
                }
            }
            brelse(bp);
        }
        current_cluster = next_cluster(dev, current_cluster);
        cluster_count++;
    }
    return 0;
}

struct inode* fat32_ialloc(uint dev, short type) {
    // Return a temporary inode that will be fully initialized in dirlink
    static uint temp_inum = 0x40000000;
    struct inode *ip = iget(dev, temp_inum++);
    if (temp_inum > 0x7FFFFFFF) temp_inum = 0x40000000;
    ip->fs_type = FS_FAT32;
    ip->type = type;
    ip->fat32_info.cluster = 0;
    ip->fat32_info.attr = (type == T_DIR) ? FAT32_ATTR_DIRECTORY : 0;
    ip->size = 0;
    ip->uid = 0;
    ip->gid = 0;
    ip->mode = type == T_DIR ? 0755 : 0644;
    ip->valid = 1;
    return ip;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

static void generate_83_name(char *name, char *res) {
    memset(res, ' ', 11);
    char *dot = strchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len > 8) len = 8;
    for (int i = 0; i < len; i++) {
        res[i] = to_upper(name[i]);
    }
    if (dot) {
        char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            res[8 + i] = to_upper(ext[i]);
        }
    }
}

int fat32_isdirempty(struct inode *dp) {
    if (dp->fs_type != FS_FAT32) return 0;
    
    uint current_cluster = dp->fat32_info.cluster;
    while (!is_eof(current_cluster)) {
        uint sector = cluster_to_sector(current_cluster);
        for (uint i = 0; i < fat_vol.sectors_per_cluster; i++) {
            struct buf *bp = bread(dp->dev, sector + i);
            struct fat32_dirent *entries = (struct fat32_dirent*)bp->data;
            for (uint j = 0; j < BSIZE / sizeof(struct fat32_dirent); j++) {
                if (entries[j].name[0] != 0 && entries[j].name[0] != 0xE5) {
                    // Check if it's "." or ".."
                    if (strncmp((char*)entries[j].name, ".          ", 11) == 0 ||
                        strncmp((char*)entries[j].name, "..         ", 11) == 0) {
                        continue;
                    }
                    brelse(bp);
                    return 0;
                }
            }
            brelse(bp);
        }
        current_cluster = next_cluster(dp->dev, current_cluster);
    }
    return 1;
}

static void free_cluster_chain(int dev, uint cluster) {
    while (!is_eof(cluster) && cluster != 0) {
        uint next = next_cluster(dev, cluster);
        set_next_cluster(dev, cluster, 0); // Mark as free
        cluster = next;
    }
}

int fat32_unlink(struct inode *dp, uint off) {
    if (dp->fs_type != FS_FAT32) return -1;
    
    uint current_cluster = dp->fat32_info.cluster;
    uint bytes_per_cluster = fat_vol.sectors_per_cluster * BSIZE;
    uint total_off = 0;

    while (off >= total_off + bytes_per_cluster) {
        current_cluster = next_cluster(dp->dev, current_cluster);
        if (is_eof(current_cluster)) return -1;
        total_off += bytes_per_cluster;
    }
    
    uint rel_off = off - total_off;
    uint sector = cluster_to_sector(current_cluster) + (rel_off / BSIZE);
    uint sec_off = rel_off % BSIZE;
    
    struct buf *bp = bread(dp->dev, sector);
    struct fat32_dirent *de = (struct fat32_dirent*)(bp->data + sec_off);
    
    uint cluster = ((uint)de->first_cluster_high << 16) | de->first_cluster_low;
    free_cluster_chain(dp->dev, cluster);
    
    de->name[0] = 0xE5; // Mark as deleted
    log_write(bp);
    brelse(bp);
    
    return 0;
}

int fat32_dirlink(struct inode *dp, char *name, struct inode *ip) {
    if (dp->fs_type != FS_FAT32) return -1;
    
    // If it's a directory, allocate its first cluster now so we can add . and ..
    if (ip->type == T_DIR && ip->fat32_info.cluster == 0) {
        ip->fat32_info.cluster = alloc_cluster(dp->dev);
        
        // Add . and ..
        struct fat32_dirent de;
        memset(&de, 0, sizeof(de));
        memset(de.name, ' ', 11);
        de.name[0] = '.';
        de.attr = FAT32_ATTR_DIRECTORY;
        de.first_cluster_high = (ip->fat32_info.cluster >> 16) & 0xFFFF;
        de.first_cluster_low = ip->fat32_info.cluster & 0xFFFF;
        fat32_write(dp->dev, &ip->fat32_info.cluster, (char*)&de, 0, sizeof(de));
        
        memset(&de, 0, sizeof(de));
        memset(de.name, ' ', 11);
        de.name[0] = '.'; de.name[1] = '.';
        de.attr = FAT32_ATTR_DIRECTORY;
        uint parent_cluster = dp->fat32_info.cluster;
        if (parent_cluster == fat32_root_cluster()) parent_cluster = 0; // FAT32 convention
        de.first_cluster_high = (parent_cluster >> 16) & 0xFFFF;
        de.first_cluster_low = parent_cluster & 0xFFFF;
        fat32_write(dp->dev, &ip->fat32_info.cluster, (char*)&de, sizeof(de), sizeof(de));
    }

    // Find empty slot
    uint current_cluster = dp->fat32_info.cluster;
    uint bytes_per_cluster = fat_vol.sectors_per_cluster * BSIZE;
    uint total_off = 0;

    while (1) {
        uint sector = cluster_to_sector(current_cluster);
        for (uint i = 0; i < fat_vol.sectors_per_cluster; i++) {
            struct buf *bp = bread(dp->dev, sector + i);
            struct fat32_dirent *entries = (struct fat32_dirent*)bp->data;
            for (uint j = 0; j < BSIZE / sizeof(struct fat32_dirent); j++) {
                if (entries[j].name[0] == 0 || entries[j].name[0] == 0xE5) {
                    // Found a slot
                    memset(&entries[j], 0, sizeof(struct fat32_dirent));
                    generate_83_name(name, (char*)entries[j].name);
                    entries[j].attr = ip->fat32_info.attr;
                    entries[j].first_cluster_high = (ip->fat32_info.cluster >> 16) & 0xFFFF;
                    entries[j].first_cluster_low = ip->fat32_info.cluster & 0xFFFF;
                    entries[j].file_size = ip->size;
                    
                    log_write(bp);
                    brelse(bp);
                    
                    ip->fat32_info.parent_cluster = dp->fat32_info.cluster;
                    ip->fat32_info.dir_off = total_off + i * BSIZE + j * sizeof(struct fat32_dirent);
                    return 0;
                }
            }
            brelse(bp);
        }
        uint next = next_cluster(dp->dev, current_cluster);
        if (is_eof(next)) {
            next = alloc_cluster(dp->dev);
            set_next_cluster(dp->dev, current_cluster, next);
        }
        current_cluster = next;
        total_off += bytes_per_cluster;
    }
}

void fat32_iupdate(struct inode *ip) {
    if (ip->fs_type != FS_FAT32 || ip->inum == fat32_root_cluster()) return;
    
    uint current_cluster = ip->fat32_info.parent_cluster;
    uint off = ip->fat32_info.dir_off;
    uint bytes_per_cluster = fat_vol.sectors_per_cluster * BSIZE;
    
    // Skip clusters to reach the offset
    while (off >= bytes_per_cluster) {
        current_cluster = next_cluster(ip->dev, current_cluster);
        if (is_eof(current_cluster)) return; // Should not happen if dir_off is valid
        off -= bytes_per_cluster;
    }
    
    uint sector = cluster_to_sector(current_cluster) + (off / BSIZE);
    uint sec_off = off % BSIZE;
    
    struct buf *bp = bread(ip->dev, sector);
    struct fat32_dirent *de = (struct fat32_dirent*)(bp->data + sec_off);
    
    de->file_size = ip->size;
    de->first_cluster_high = (ip->fat32_info.cluster >> 16) & 0xFFFF;
    de->first_cluster_low = ip->fat32_info.cluster & 0xFFFF;
    de->attr = ip->fat32_info.attr;
    
    log_write(bp);
    brelse(bp);
}

int fat32_read(int dev, uint start_cluster, char *dst, uint off, uint n) {
    if (!fat_vol.valid) return -1;
    
    uint current_cluster = start_cluster;
    uint bytes_per_cluster = fat_vol.sectors_per_cluster * BSIZE;
    
    // Skip clusters to reach the offset
    while (off >= bytes_per_cluster && !is_eof(current_cluster)) {
        current_cluster = next_cluster(dev, current_cluster);
        off -= bytes_per_cluster;
    }
    
    if (is_eof(current_cluster)) return 0;
    
    uint tot = 0;
    while (n > 0 && !is_eof(current_cluster)) {
        uint sector = cluster_to_sector(current_cluster);
        uint cluster_off = off;
        uint cluster_rem = bytes_per_cluster - cluster_off;
        uint to_read = (n > cluster_rem) ? cluster_rem : n;
        
        // Read sectors within this cluster
        for (uint i = cluster_off / BSIZE; i < fat_vol.sectors_per_cluster && to_read > 0; i++) {
            struct buf *bp = bread(dev, sector + i);
            uint sec_off = cluster_off % BSIZE;
            uint sec_rem = BSIZE - sec_off;
            uint chunk = (to_read > sec_rem) ? sec_rem : to_read;
            memmove(dst, bp->data + sec_off, chunk);
            brelse(bp);
            
            dst += chunk;
            n -= chunk;
            tot += chunk;
            cluster_off = 0;
        }
        
        current_cluster = next_cluster(dev, current_cluster);
        off = 0;
    }
    return tot;
}

int fat32_write(int dev, uint *start_cluster, char *src, uint off, uint n) {
    if (!fat_vol.valid) return -1;
    
    if (*start_cluster == 0) {
        *start_cluster = alloc_cluster(dev);
    }
    
    uint current_cluster = *start_cluster;
    uint bytes_per_cluster = fat_vol.sectors_per_cluster * BSIZE;
    
    // Skip clusters to reach the offset
    while (off >= bytes_per_cluster) {
        uint next = next_cluster(dev, current_cluster);
        if (is_eof(next)) {
            next = alloc_cluster(dev);
            set_next_cluster(dev, current_cluster, next);
        }
        current_cluster = next;
        off -= bytes_per_cluster;
    }
    
    uint tot = 0;
    while (n > 0) {
        uint sector = cluster_to_sector(current_cluster);
        uint cluster_off = off;
        uint cluster_rem = bytes_per_cluster - cluster_off;
        uint to_write = (n > cluster_rem) ? cluster_rem : n;
        
        // Write sectors within this cluster
        for (uint i = cluster_off / BSIZE; i < fat_vol.sectors_per_cluster && to_write > 0; i++) {
            struct buf *bp = bread(dev, sector + i);
            uint sec_off = cluster_off % BSIZE;
            uint sec_rem = BSIZE - sec_off;
            uint chunk = (to_write > sec_rem) ? sec_rem : to_write;
            memmove(bp->data + sec_off, src, chunk);
            log_write(bp);
            brelse(bp);
            
            src += chunk;
            n -= chunk;
            tot += chunk;
            cluster_off = 0;
        }
        
        if (n > 0) {
            uint next = next_cluster(dev, current_cluster);
            if (is_eof(next)) {
                next = alloc_cluster(dev);
                set_next_cluster(dev, current_cluster, next);
            }
            current_cluster = next;
            off = 0;
        }
    }
    return tot;
}
