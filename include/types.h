#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdbool.h>

typedef enum newfs_file_type {
    NEWFS_REG_FILE,
    NEWFS_DIR,
    NEWFS_SYM_LINK // Symbolic link
} NEWFS_FILE_TYPE;

struct custom_options {
	const char*        device;
};

/******************************************************************************
* SECTION: Macro
*******************************************************************************/
#define UINT32_BITS             32
#define UINT8_BITS              8

#define NEWFS_ROOT_INO          0

#define MAX_NAME_LEN            128
#define NEWFS_INODE_PER_FILE    1 
#define NEWFS_DATA_PER_FILE     1024    /* 每个文件最多使用的数据块数 */

#define NEWFS_ERROR_NONE        0
#define NEWFS_ERROR_NOSPACE     ENOSPC
#define NEWFS_ERROR_IO          EIO     /* Error Input/Output */
#define NEWFS_ERROR_EXISTS      EEXIST  /* File exists */
#define NEWFS_ERROR_UNSUPPORTED ENXIO   /* Unsupported operation */
#define NEWFS_ERROR_NOTFOUND    ENOENT  /* No such file or directory */
#define NEWFS_ERROR_ACCESS      EACCES  /* Permission denied */
#define NEWFS_ERROR_ISDIR       EISDIR  /* Is a directory */

/******************************************************************************
* SECTION: Macro Function
*******************************************************************************/
#define NEWFS_IO_SZ()                     (super.blks_size)
#define NEWFS_DISK_SZ()                   (super.disk_size)
#define NEWFS_DRIVER()                    (super.fd)

#define NEWFS_BLKS_SZ(blks)               ((blks) * NEWFS_IO_SZ())
#define NEWFS_ASSIGN_FNAME(psfs_dentry, _fname)     memcpy(psfs_dentry->name, _fname, strlen(_fname))

#define NEWFS_DATA_OFS(p)                 (super.data_offset + (p) * NEWFS_IO_SZ())

#define NEWFS_IS_DIR(pinode)              (pinode->ftype == NEWFS_DIR)
#define NEWFS_IS_REG(pinode)              (pinode->ftype == NEWFS_REG_FILE)
#define NEWFS_IS_SYM_LINK(pinode)         (pinode->ftype == NEWFS_SYM_LINK)

/******************************************************************************
* SECTION: FS Specific Structure - In memory structure
*******************************************************************************/
struct newfs_super {
    uint32_t magic;
    int      fd;
    /* TODO: Define yourself */
    int disk_size;        // 磁盘大小
    /* 逻辑块信息 */
    int blks_size;          // 逻辑块大小

    /* 磁盘布局分区信息 */
    int sb_offset;          // 超级块于磁盘中的偏移，通常默认为0
    int sb_blks;            // 超级块于磁盘中的块数，通常默认为1

    int ino_map_offset;     // 索引节点位图于磁盘中的偏移
    int ino_map_blks;       // 索引节点位图于磁盘中的块数
    uint8_t* ino_bitmap;      // 索引节点位图内存镜像

    int dat_map_offset;     // 数据块位图于磁盘中的偏移
    int dat_map_blks;       // 数据块位图于磁盘中的块数
    uint8_t* data_bitmap;     // 数据块位图内存镜像

    int inode_offset;       // 索引节点区同理
    int inode_blks;
    
    int data_offset;        // 数据块区同理
    int data_blks;

    /* 支持的限制 */
    int ino_max;            // 最大支持inode数
    int file_max;           // 支持文件最大大小

    /* 根目录索引 */
    int root_ino;           // 根目录对应的inode
    struct newfs_dentry* root_dentry;  // 根目录对应的dentry

    /* 其他信息 */
    bool is_mounted;        // 是否已挂载

};

struct newfs_inode {
    uint32_t ino;
    /* TODO: Define yourself */
    int                size;                          /* 文件已占用空间 */
    // char               target_path[MAX_NAME_LEN];     /* store target path when it is a symlink */
    int                dir_cnt;                       /* 目录项个数，当文件类型为目录时有效 */
    NEWFS_FILE_TYPE    ftype;                         /* 文件类型 */
    struct newfs_dentry* dentry;                      /* 指向该inode的dentry，即inode的母目录 */
    struct newfs_dentry* dentrys;                     /* 如果文件类型为目录，它的所有目录项 */
    uint8_t*           data_blks[NEWFS_DATA_PER_FILE];/* 指向数据块的指针 */
    uint32_t           data[NEWFS_DATA_PER_FILE];     /* 数据块号 */
};

struct newfs_dentry {
    char     name[MAX_NAME_LEN];
    uint32_t ino;
    /* TODO: Define yourself */
    NEWFS_FILE_TYPE      ftype;
    struct newfs_inode*  inode;
    struct newfs_dentry* parent;
    struct newfs_dentry* brother;
};

static inline struct newfs_dentry* new_dentry(char * fname, NEWFS_FILE_TYPE ftype) {
    struct newfs_dentry * dentry = (struct newfs_dentry *)malloc(sizeof(struct newfs_dentry));
    memset(dentry, 0, sizeof(struct newfs_dentry));
    NEWFS_ASSIGN_FNAME(dentry, fname);
    dentry->ino     = -1;
    dentry->ftype   = ftype;
    dentry->inode   = NULL;
    dentry->parent  = NULL;
    dentry->brother = NULL;
    return dentry;                                       
}

/******************************************************************************
* SECTION: FS Specific Structure - Disk structure
*******************************************************************************/

struct newfs_super_d {
    uint32_t magic;

    /* 磁盘布局分区信息 */
    int sb_offset;          // 超级块于磁盘中的偏移，通常默认为0
    int sb_blks;            // 超级块于磁盘中的块数，通常默认为1

    int ino_map_offset;     // 索引节点位图于磁盘中的偏移
    int ino_map_blks;       // 索引节点位图于磁盘中的块数

    int dat_map_offset;     // 数据块位图于磁盘中的偏移
    int dat_map_blks;       // 数据块位图于磁盘中的块数

    int inode_offset;       // 索引节点区同理
    int inode_blks;
    
    int data_offset;        // 数据块区同理
    int data_blks;

    /* 支持的限制 */
    int ino_max;            // 最大支持inode数
    int file_max;           // 支持文件最大大小

    /* 根目录索引 */
    int root_ino;           // 根目录对应的inode

    /* 其他信息 */

};

struct newfs_inode_d {
    uint32_t ino;
    /* TODO: Define yourself */
    int                size;                          /* 文件已占用空间 */
    // char               target_path[MAX_NAME_LEN];     /* store target path when it is a symlink */
    int                dir_cnt;                       /* 目录项个数，当文件类型为目录时有效 */
    NEWFS_FILE_TYPE    ftype;                         /* 文件类型 */
    uint32_t           data[NEWFS_DATA_PER_FILE];     /* 数据块号 */
};

struct newfs_dentry_d {
    char     name[MAX_NAME_LEN];
    uint32_t ino;
    NEWFS_FILE_TYPE      ftype;
};

#endif /* _TYPES_H_ */