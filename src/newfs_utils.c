#include "newfs.h"
#include <stdbool.h>

extern struct custom_options newfs_options;
extern struct newfs_super    super;

/**
 * @brief 封装对ddriver的访问代码
 * @param offset 磁盘偏移
 * @param out_content 读出/写入的内容
 * @param size 读出/写入大小
 * @return int 0成功，否则返回错误码
 */
int your_read(int offset, void *out_content, int size) {
	// 确定要读取的上界和下界
    int io_sz = NEWFS_IO_SZ() / 2; //512B
	long up = (offset + size + io_sz - 1) / io_sz * io_sz;	//向上取整
	long down = offset / io_sz * io_sz;					//向下取整
	// 读出从down到up的磁盘块到内存
	ddriver_seek(super.fd, down, SEEK_SET);
	char temp[up - down];
	int blk_num = (up - down) / io_sz;
	for (int i = 0; i < blk_num; i++) {
		ddriver_read(super.fd, temp + i * io_sz, io_sz);
	}
	// 从内存拷贝到out_content
	memcpy(out_content, temp + (offset - down), size);
	return 0;
}

int your_write(int offset, void *out_content, int size) {
	// 确定要写的上界和下界
    int io_sz = NEWFS_IO_SZ() / 2;
	long up = (offset + size + io_sz - 1) / io_sz * io_sz;	//向上取整
	long down = offset / io_sz * io_sz;					//向下取整
	// 读出从down到up的磁盘块到内存
	ddriver_seek(super.fd, down, SEEK_SET);
	char temp[up - down];
	int blk_num = (up - down) / io_sz;
	for (int i = 0; i < blk_num; i++) {
		ddriver_read(super.fd, temp + i * io_sz, io_sz);
	}
	// 修改阶段，在内存覆盖指定内容
	memcpy(temp + (offset - down), out_content, size);
	// 写回磁盘
	ddriver_seek(super.fd, down, SEEK_SET);
	for (int i = 0; i < blk_num; i++) {
		ddriver_write(super.fd, temp + i * io_sz, io_sz);
	}
	return 0;
} 

/**
 * @brief 分配一个inode，占用位图
 * 
 * @param dentry 该dentry指向分配的inode
 * @return struct newfs_inode* 
 */
struct newfs_inode* newfs_alloc_inode(struct newfs_dentry * dentry) {
	struct newfs_inode* inode;
	int byte_cursor = 0;
	int bit_cursor = 0;
	int ino_cursor = 0;
	bool is_found_free_entry = false;
	// 在inode位图中寻找空闲的inode位置
	for (byte_cursor = 0; byte_cursor < NEWFS_BLKS_SZ(super.ino_map_blks); 
         byte_cursor++)
    {
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((super.ino_bitmap[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                                                      /* 当前ino_cursor位置空闲 */
                super.ino_bitmap[byte_cursor] |= (0x1 << bit_cursor);
				printf("allocate inode %d\n", ino_cursor);
                is_found_free_entry = true;           
                break;
            }
            ino_cursor++;
        }
        if (is_found_free_entry) {
            break;
        }
    }

	if (!is_found_free_entry || ino_cursor >= super.ino_max)
        return NULL;    /* 未找到空闲inode位置 */

    inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));

    inode->ino  = ino_cursor;
    inode->size = 0;
    inode->dir_cnt = 0;
    inode->dentrys = NULL;

    /* dentry指向inode */
    dentry->inode = inode;
    dentry->ino   = inode->ino;
    
	/* inode指回dentry */
    inode->dentry = dentry;
    
    inode->ftype = dentry->ftype;

    for (int i = 0; i < NEWFS_DATA_PER_FILE; i++){
        inode->data[i] = -1;
        inode->data_blks[i] = (uint8_t *)malloc(NEWFS_IO_SZ());
    }

    return inode;
}

/**
 * @brief 将内存inode及其下方结构全部刷回磁盘
 * 
 * @param inode 
 * @return int 
 */
int newfs_sync_inode(struct newfs_inode * inode) {
    struct newfs_inode_d  inode_d;
    struct newfs_dentry*  dentry_cursor;
    struct newfs_dentry_d dentry_d;
    int offset;
    int ino             = inode->ino;

    // 填充inode_d结构
    inode_d.ino         = ino;
    inode_d.size        = inode->size;
    // memcpy(inode_d.target_path, inode->target_path, MAX_NAME_LEN);
    inode_d.ftype       = inode->dentry->ftype;
    inode_d.dir_cnt     = inode->dir_cnt;
    for (int i = 0; i < NEWFS_DATA_PER_FILE; i++) {
        inode_d.data[i] = inode->data[i];
    }

    /* 先写inode本身 */
    int inodes_per_block = NEWFS_IO_SZ() / sizeof(struct newfs_inode_d);
    int off = ino / inodes_per_block;
    int idx = ino % inodes_per_block;
    int inode_offset = super.inode_offset + off * NEWFS_IO_SZ() + idx * sizeof(struct newfs_inode_d);
    if (your_write(inode_offset, (uint8_t *)&inode_d, 
                    sizeof(struct newfs_inode_d)) != NEWFS_ERROR_NONE) {
        NEWFS_DBG("[%s] inode io error\n", __func__);
        return -NEWFS_ERROR_IO;
    }
	/* 再写inode下方的数据 */
    if (NEWFS_IS_DIR(inode)) { /* 如果当前inode是目录，那么数据是目录项，且目录项的inode也要写回 */                          
        dentry_cursor = inode->dentrys;
        int max_dentries_per_block = NEWFS_IO_SZ() / sizeof(struct newfs_dentry_d);

        int dentry_index = 0;
        
        while (dentry_cursor != NULL) {
            int current_block = dentry_index / max_dentries_per_block;
            int index_in_block = dentry_index % max_dentries_per_block;
            
            if (current_block >= NEWFS_DATA_PER_FILE) {
                NEWFS_DBG("[%s] too many data blocks\n", __func__);
                return -NEWFS_ERROR_IO;
            }
            
            offset = NEWFS_DATA_OFS(inode->data[current_block]) + 
                    index_in_block * sizeof(struct newfs_dentry_d);
            
            memcpy(dentry_d.name, dentry_cursor->name, MAX_NAME_LEN);
            dentry_d.ftype = dentry_cursor->ftype;
            dentry_d.ino   = dentry_cursor->ino;
            
            if (your_write(offset, (uint8_t *)&dentry_d, 
                                sizeof(struct newfs_dentry_d)) != NEWFS_ERROR_NONE) {
                NEWFS_DBG("[%s] dentry io error\n", __func__);
                return -NEWFS_ERROR_IO;            
            }
            
            if (dentry_cursor->inode != NULL) {
                newfs_sync_inode(dentry_cursor->inode);
            }

            dentry_cursor = dentry_cursor->brother;
            dentry_index++;
        }
        
        // 验证目录项数量是否匹配
        if (dentry_index != inode->dir_cnt) {
            NEWFS_DBG("[%s] dentry count mismatch: expected %d, got %d\n", 
                    __func__, inode->dir_cnt, dentry_index);
            return -NEWFS_ERROR_IO;
        }
    } else if (NEWFS_IS_REG(inode)) { /* 如果当前inode是文件，那么数据是文件内容，直接写即可 */
        for (int i = 0; i < NEWFS_DATA_PER_FILE; i++) {
            if( inode->data[i] == -1 ) {
                break;
            }
            offset = NEWFS_DATA_OFS(inode->data[i]);
            your_write(offset, inode->data_blks[i], NEWFS_IO_SZ());
        }
    }
    return NEWFS_ERROR_NONE;
}

/**
 * @brief 将denry插入到inode中，采用头插法
 * 
 * @param inode 
 * @param dentry 
 * @return int 
 */
int newfs_alloc_dentry(struct newfs_inode* inode, struct newfs_dentry* dentry) {
    if (inode->dentrys == NULL) {
        inode->dentrys = dentry;
    }
    else {
        dentry->brother = inode->dentrys;
        inode->dentrys = dentry;
    }

    inode->size += sizeof(struct newfs_dentry_d);

    const int per_blk = NEWFS_IO_SZ() / sizeof(struct newfs_dentry_d);
    int cur_dir_cnt = inode->dir_cnt; // 当前子项数量

    if(cur_dir_cnt >= NEWFS_DATA_PER_FILE * ( NEWFS_IO_SZ() / sizeof(struct newfs_dentry_d))) {
        return -1; /* 超过单目录支持的最大文件大小 */
    }

    // 如果当前目录条目正好填满了现有块集合（或首次插入时需要至少一个块），则要申请新块
    bool need_alloc_block = (cur_dir_cnt == 0) || (cur_dir_cnt % per_blk == 0);
    if (need_alloc_block) {
        int byte_cursor = 0;
        int bit_cursor = 0;
        int data_blk_cursor = 0;
        bool is_found_free_entry = false;
        // 在数据块位图中寻找空闲的数据块位置
        for (byte_cursor = 0; byte_cursor < NEWFS_BLKS_SZ(super.dat_map_blks); 
             byte_cursor++)
        {
            for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
                if((super.data_bitmap[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                                                          /* 当前data_blk_cursor位置空闲 */
                    super.data_bitmap[byte_cursor] |= (0x1 << bit_cursor);
                    is_found_free_entry = true;           
                    break;
                }
                data_blk_cursor++;
            }
            if (is_found_free_entry) {
                break;
            }
        }

        if (!is_found_free_entry || data_blk_cursor >= super.data_blks)
            return -1;    /* 未找到空闲数据块位置 */

        // 分配新数据块给inode
        int new = cur_dir_cnt / per_blk;
        inode->data[new] = data_blk_cursor;
        inode->data_blks[new] = (uint8_t *)malloc(NEWFS_IO_SZ());
        memset(inode->data_blks[new], 0, NEWFS_IO_SZ());
    }

    inode->dir_cnt++;

    return inode->dir_cnt;
}

/**
 * @brief 从磁盘中读取inode节点
 * 
 * @param dentry dentry指向ino，读取该inode
 * @param ino inode唯一编号
 * @return struct newfs_inode* 
 */
struct newfs_inode* newfs_read_inode(struct newfs_dentry * dentry, int ino){
	struct newfs_inode* inode = (struct newfs_inode *)malloc(sizeof(struct newfs_inode));
	struct newfs_inode_d inode_d;
    struct newfs_dentry* sub_dentry;
    struct newfs_dentry_d dentry_d;
	int    dir_cnt = 0, i = 0;

	// 计算inode在磁盘中的偏移
    int inodes_per_block = NEWFS_IO_SZ() / sizeof(struct newfs_inode_d);
    int off = (ino / inodes_per_block);
    int idx = ino % inodes_per_block;
	int inode_offset = super.inode_offset + off * NEWFS_IO_SZ() + idx * sizeof(struct newfs_inode_d);
	// 读取inode到内存
	your_read(inode_offset, &inode_d, sizeof(struct newfs_inode_d));

	inode->dir_cnt = 0;
	inode->ino = inode_d.ino;
	inode->size = inode_d.size;
	inode->ftype = inode_d.ftype;
	// memcpy(inode->target_path, inode_d.target_path, SFS_MAX_FILE_NAME);
    inode->dentry = dentry;
    inode->dentrys = NULL;
	for (i = 0; i < NEWFS_DATA_PER_FILE; i++) {
        inode->data[i] = inode_d.data[i];
        // inode->data_blks[i] = NULL;
    }

	/* 内存中的inode的数据或子目录项部分也需要读出 */
	if (NEWFS_IS_DIR(inode)) {
		// 读取目录项
        inode->dir_cnt = inode_d.dir_cnt;
		dir_cnt = inode->dir_cnt;
        int i = 0, j;
        while (inode_d.data[i] != -1 && i < NEWFS_DATA_PER_FILE) {
            int dentry_block = inode->data[i];
            int max_dentries_per_block = NEWFS_IO_SZ() / sizeof(struct newfs_dentry_d);

            for (j = 0; j < max_dentries_per_block; j++) {
                if (i * max_dentries_per_block + j >= dir_cnt) {
                    break;  /* 读完所有目录项 */
                }
                int offset_in_block = j;
                // 计算目录项在磁盘中的偏移
                int dentry_offset = NEWFS_DATA_OFS(dentry_block) + offset_in_block * sizeof(struct newfs_dentry_d);
                // 读取目录项到内存
                your_read(dentry_offset, &dentry_d, sizeof(struct newfs_dentry_d));

                sub_dentry = new_dentry(dentry_d.name, dentry_d.ftype);
                sub_dentry->ino    = dentry_d.ino;
                sub_dentry->parent = inode->dentry;
                newfs_alloc_dentry(inode, sub_dentry);
            }
            i++;
        }
	} else if (NEWFS_IS_REG(inode)) {
        // 读取文件数据
        for(int blk_cnt = 0; blk_cnt < NEWFS_DATA_PER_FILE; blk_cnt++){
            if(inode->data[blk_cnt] == -1) break;
            inode->data_blks[blk_cnt] = (uint8_t *)malloc(NEWFS_IO_SZ());
            if (your_read(NEWFS_DATA_OFS(inode->data[blk_cnt]), inode->data_blks[blk_cnt], 
                                NEWFS_IO_SZ()) != NEWFS_ERROR_NONE) {
                NEWFS_DBG("[%s] io error\n", __func__);
                return NULL;                    
            }
        }

	}
	return inode;
}

/**
 * @brief 计算路径的层级
 * exm: /av/c/d/f
 * -> lvl = 4
 * @param path 
 * @return int 
 */
int newfs_calc_lvl(const char * path) {
    int   lvl = 0;
    if (strcmp(path, "/") == 0) {
        return lvl;
    }
    for (const char *str = path; *str!= '\0'; str++) {
        if (*str == '/') {
            lvl++;
        }
    }
    return lvl;
}

/**
 * @brief 查找文件或目录
 * path: /qwe/ad  total_lvl = 2,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry 
 *      3) find qwe's inode     lvl = 2
 *      4) find ad's dentry
 *
 * path: /qwe     total_lvl = 1,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry
 *  
 * 
 * 如果能查找到，返回该目录项
 * 如果查找不到，返回的是上一个有效的路径
 * 
 * path: /a/b/c
 *      1) find /'s inode     lvl = 1
 *      2) find a's dentry 
 *      3) find a's inode     lvl = 2
 *      4) find b's dentry    如果此时找不到了，is_find=FALSE且返回的是a的inode对应的dentry
 * 
 * @param path 
 * @param is_find 找到则为TRUE，未找到则为FALSE
 * @param is_root 是否为根目录
 * @return struct newfs_dentry* 
 */
struct newfs_dentry* newfs_lookup(const char * path, bool* is_find, bool* is_root) {
    struct newfs_dentry* dentry_cursor = super.root_dentry;
    struct newfs_dentry* dentry_ret = NULL;
    struct newfs_inode*  inode; 
    int   total_lvl = newfs_calc_lvl(path);
    int   lvl = 0;
    bool  is_hit;
    char* fname = NULL;
    char* path_cpy = (char*)malloc(strlen(path));
	*is_root = false;
    strcpy(path_cpy, path);

    if (total_lvl == 0) {                           	/* 根目录 */
		*is_find = true;
		*is_root = true;
        dentry_ret = super.root_dentry;
		return dentry_ret;
    }
	fname = strtok(path_cpy, "/");   
    while (fname)
    {   
        lvl++;
        if (dentry_cursor->inode == NULL) {             /* Cache机制 */
            newfs_read_inode(dentry_cursor, dentry_cursor->ino);
        }

        inode = dentry_cursor->inode;

        if (NEWFS_IS_REG(inode) && lvl < total_lvl) {
			// 是文件类型但是还没到最后一级，报错
            NEWFS_DBG("[%s] not a dir\n", __func__);
            dentry_ret = inode->dentry;
            break;
        }
        if (NEWFS_IS_DIR(inode)) {
            dentry_cursor = inode->dentrys;
            is_hit        = false;

            while (dentry_cursor)   /* 遍历子目录项 */
            {
                if (memcmp(dentry_cursor->name, fname, strlen(fname)) == 0) {
                    is_hit = true;
                    break;
                }
                dentry_cursor = dentry_cursor->brother;
            }
            
            if (!is_hit) {
                // 未找到，返回上一级目录项
				*is_find = false;
                NEWFS_DBG("[%s] not found: %s\n", __func__, fname);
                dentry_ret = inode->dentry;
                break;
            }

            if (is_hit && lvl == total_lvl) {
				*is_find = true;
                dentry_ret = dentry_cursor;
                break;
            }
        }
        fname = strtok(NULL, "/"); 
    }

    if (dentry_ret->inode == NULL) {
        dentry_ret->inode = newfs_read_inode(dentry_ret, dentry_ret->ino);
    }
    
    return dentry_ret;
}

/**
 * @brief 获取文件名
 * 
 * @param path 
 * @return char* 
 */
char* newfs_get_fname(const char* path) {
    char ch = '/';
    char *q = strrchr(path, ch) + 1;
    return q;
}

/**
 * @brief 
 * 
 * @param inode 
 * @param dir [0...]
 * @return struct newfs_dentry* 
 */
struct newfs_dentry* newfs_get_dentry(struct newfs_inode * inode, int dir) {
    struct newfs_dentry* dentry_cursor = inode->dentrys;
    int    cnt = 0;
    while (dentry_cursor)
    {
        if (dir == cnt) {
            return dentry_cursor;
        }
        cnt++;
        dentry_cursor = dentry_cursor->brother;
    }
    return NULL;
}