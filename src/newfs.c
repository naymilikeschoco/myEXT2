#define _XOPEN_SOURCE 700

#include "newfs.h"
#include <stdbool.h>

/******************************************************************************
* SECTION: 宏定义
*******************************************************************************/
#define OPTION(t, p)        { t, offsetof(struct custom_options, p), 1 }

/******************************************************************************
* SECTION: 全局变量
*******************************************************************************/
static const struct fuse_opt option_spec[] = {		/* 用于FUSE文件系统解析参数 */
	OPTION("--device=%s", device),
	FUSE_OPT_END
};

struct custom_options newfs_options;			 /* 全局选项 */
struct newfs_super super;

/******************************************************************************
* SECTION: FUSE操作定义
*******************************************************************************/
static struct fuse_operations operations = {
	.init = newfs_init,						 /* mount文件系统 */		
	.destroy = newfs_destroy,				 /* umount文件系统 */
	.mkdir = newfs_mkdir,					 /* 建目录，mkdir */
	.getattr = newfs_getattr,				 /* 获取文件属性，类似stat，必须完成 */
	.readdir = newfs_readdir,				 /* 填充dentrys */
	.mknod = newfs_mknod,					 /* 创建文件，touch相关 */
	.write = NULL,								  	 /* 写入文件 */
	.read = NULL,								  	 /* 读文件 */
	.utimens = newfs_utimens,				 /* 修改时间，忽略，避免touch报错 */
	.truncate = NULL,						  		 /* 改变文件大小 */
	.unlink = NULL,							  		 /* 删除文件 */
	.rmdir	= NULL,							  		 /* 删除目录， rm -r */
	.rename = NULL,							  		 /* 重命名，mv */

	.open = NULL,							
	.opendir = NULL,
	.access = NULL
};
/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 文件系统布局:
 * 磁盘容量为4MB，逻辑块大小为1024B，那么逻辑块数为4MB / 1024B = 4096块
 * 假设每个文件最多直接索引(*5)个逻辑块来填写文件数据，每个文件数据上限是 5 * 1024B = 5KB。
 * 假设一个文件的索引节点大小为(*36B)，那么每个逻辑块可以存放1024B / (*36B) = (*28)个索引节点。
 * @attention 文件索引结点大小最后根据数据结构体struct inode_d的实际大小来定！！！
 * 维护一个文件所需要的存储容量是，那么4MB磁盘，最多可以存放的文件数是4MB / (5KB + (*36B)) = (*812)。
 * super block: 1个逻辑块
 * 索引结点位图：1个逻辑块（位图可以记录8192个索引结点的使用情况，远大于(*812)个）
 * 数据块位图：1个逻辑块（位图可以记录8192个数据块的使用情况，远大于(*4065)个）
 * 索引结点区：(*29)个逻辑块。一个逻辑块放(*28)个索引节点struct inode_d，(*812)个文件，也就是 (*812) / (*28) = (*29)个逻辑块。
 * 数据块区：剩余的逻辑块，即4096 - 1 - 1 - 1 - (*29) = (*4064)个逻辑块
 * 
 * 其中目录项为136B大小，那么每个逻辑块可以存放1024B / 136B = 7个目录项。一个inode最多5*7=35个目录项。
*/

/**
 * @brief 挂载（mount）文件系统
 * 
 * @param conn_info 可忽略，一些建立连接相关的信息 
 * @return void*
 */
void* newfs_init(struct fuse_conn_info * conn_info) {
	/* TODO: 在这里进行挂载 */
	/*定义磁盘各部分结构*/
    int                 	driver_fd;
    struct newfs_super_d  	newfs_super_d; 
    struct newfs_dentry*  	root_dentry;
    struct newfs_inode*   	root_inode;

    super.is_mounted = false;

	// 打开设备
	driver_fd = ddriver_open((char*)newfs_options.device);
	if (driver_fd < 0) {
		// 错误处理：无法打开设备
		return NULL;
	}

	// 向内存超级块中标记驱动并写入磁盘大小和单次IO大小
	super.fd = driver_fd;
	ddriver_ioctl(super.fd, IOC_REQ_DEVICE_SIZE,  &super.disk_size); // 4MB
	int disk_io_sz;
	ddriver_ioctl(super.fd, IOC_REQ_DEVICE_IO_SZ, &disk_io_sz);
	super.blks_size = 2 * disk_io_sz; // 逻辑块大小1024B
   	// 读取磁盘超级块到内存
	your_read(0, &newfs_super_d, sizeof(struct newfs_super_d));
 
	if(newfs_super_d.magic != NEWFS_MAGIC) {
		/* 第一次挂载 */
        /* 将上述估算思路用代码实现 */

        /* 填充超级块的磁盘布局信息字段 */ 
		// step 1: 重新估计磁盘布局信息 
		int blk = super.blks_size;   // 逻辑块大小1024B
        super.sb_offset = 0;           // 超级块于磁盘中的偏移，默认为0
        super.sb_blks = 1;             // 超级块于磁盘中的逻辑块数

        super.ino_map_offset = blk;      // 索引节点位图于磁盘中的偏移(1个逻辑块)
        super.ino_map_blks = 1;        // 索引节点位图于磁盘中的块数

        super.dat_map_offset = 2*blk;      // 数据块位图于磁盘中的偏移(2个逻辑块)
		super.dat_map_blks = 1;        // 数据块位图于磁盘中的块数

		super.inode_offset = 3*blk;       // 索引节点区于磁盘中的偏移(3个逻辑块)
		super.inode_blks = 29;         // 索引节点区于磁盘中的块数

		super.data_offset = 32*blk;        // 数据块区于磁盘中的偏移(29个逻辑块)
		super.data_blks = 4064;        // 数据块区于磁盘中的块数

		/* 计算支持的最大inode数和文件大小 */
		super.ino_max = super.inode_blks * (super.blks_size / sizeof(struct newfs_inode)); // 最大支持inode数
		super.file_max = NEWFS_DATA_PER_FILE * super.blks_size; // 支持文件最大大小

		/* 根目录对应的inode */
		super.root_ino = 0; // 根目录对应的inode编号为0

		// 幻数初始化
		super.magic = NEWFS_MAGIC;

		// step 2: 清零索引结点、数据块位图
		// 数据块位图全0
		super.ino_bitmap = (uint8_t *)malloc(super.blks_size);
		memset(super.ino_bitmap, 0, super.blks_size);
		your_write(super.ino_map_offset, super.ino_bitmap, super.blks_size);

		super.data_bitmap = (uint8_t *)malloc(super.blks_size);
		memset(super.data_bitmap, 0, super.blks_size);  
		your_write(super.dat_map_offset, super.data_bitmap, super.blks_size);

		// step 3: 创建空根目inode和dentry
		// 创建根目录dentry
		root_dentry     = new_dentry("/", NEWFS_DIR);
		root_dentry->ino = super.root_ino;
		root_dentry->parent = NULL; /* 根目录没有父目录 */
        root_inode = newfs_alloc_inode(root_dentry);
		if (root_inode == NULL) {
    		NEWFS_DBG("alloc root inode failed\n");
    		/* 释放并返回错误 */
    		return NULL;
		}
        newfs_sync_inode(root_inode);

		root_dentry->inode    = root_inode;

    }else {
		/* 非第一次挂载 */
		/* 读取超级块的磁盘布局信息字段到内存超级块 */
		super.sb_offset        = newfs_super_d.sb_offset;
		super.sb_blks          = newfs_super_d.sb_blks;

		super.ino_map_offset   = newfs_super_d.ino_map_offset;
		super.ino_map_blks     = newfs_super_d.ino_map_blks;

		super.dat_map_offset   = newfs_super_d.dat_map_offset;
		super.dat_map_blks     = newfs_super_d.dat_map_blks;

		super.ino_bitmap = (uint8_t *)malloc(super.blks_size);
		super.data_bitmap = (uint8_t *)malloc(super.blks_size);
		your_read(super.ino_map_offset, super.ino_bitmap, NEWFS_IO_SZ());
		your_read(super.dat_map_offset, super.data_bitmap, NEWFS_IO_SZ());

		super.inode_offset     = newfs_super_d.inode_offset;
		super.inode_blks       = newfs_super_d.inode_blks;

		super.data_offset      = newfs_super_d.data_offset;
		super.data_blks        = newfs_super_d.data_blks;

		super.ino_max          = newfs_super_d.ino_max;
		super.file_max         = newfs_super_d.file_max;
		super.root_ino         = newfs_super_d.root_ino;

		root_dentry            = new_dentry("/", NEWFS_DIR);
		root_dentry->ino       = super.root_ino;
		root_dentry->parent    = NULL;
		root_inode             = newfs_read_inode(root_dentry, NEWFS_ROOT_INO);  /* 读取根目录 */
		root_dentry->inode     = root_inode;
	}

	super.root_dentry 	  = root_dentry;
	super.is_mounted      = true;
	
	printf("ino bitmap:\n");
	newfs_dump_map(super.ino_bitmap); /* 调试：打印位图 */
	printf("data bitmap:\n");
	newfs_dump_map(super.data_bitmap);
	return NULL;
}

/**
 * @brief 卸载（umount）文件系统
 * 
 * @param p 可忽略
 * @return void
 */
void newfs_destroy(void* p) {
	/* TODO: 在这里进行卸载 */
	struct newfs_super_d  	newfs_super_d;
	int ret; 
	/* 将超级块写入磁盘 */
	// 将内存超级块信息复制到磁盘超级块
	if (!super.is_mounted) {
        return ;
    }

	/* 1）刷写所有inode & 数据 */
	if (super.root_dentry && super.root_dentry->inode) {
		ret = newfs_sync_inode(super.root_dentry->inode);
		if (ret < 0) {
			NEWFS_DBG("[%s] newfs_destroy: sync root inode failed\n", __func__);
		}
	}
	
	/* 2）写回位图 */
	ret = your_write(super.ino_map_offset, super.ino_bitmap, NEWFS_IO_SZ());
	if (ret < 0) {
        NEWFS_DBG("[%s] newfs_destroy: write ino_bitmap failed\n", __func__);
    }

	ret = your_write(super.dat_map_offset, super.data_bitmap, NEWFS_IO_SZ());
	if (ret < 0) {
        NEWFS_DBG("[%s] newfs_destroy: write data_bitmap failed\n", __func__);
    }

	/* 3）写回超级块 */
	memset(&newfs_super_d, 0, sizeof(newfs_super_d));
	newfs_super_d.magic = NEWFS_MAGIC;
	newfs_super_d.sb_offset = super.sb_offset;
	newfs_super_d.sb_blks = super.sb_blks;
	newfs_super_d.ino_map_offset = super.ino_map_offset;
	newfs_super_d.ino_map_blks = super.ino_map_blks;
	newfs_super_d.dat_map_offset = super.dat_map_offset;
	newfs_super_d.dat_map_blks = super.dat_map_blks;
	newfs_super_d.inode_offset = super.inode_offset;
	newfs_super_d.inode_blks = super.inode_blks;
	newfs_super_d.data_offset = super.data_offset;
	newfs_super_d.data_blks = super.data_blks;
	newfs_super_d.ino_max = super.ino_max;
	newfs_super_d.file_max = super.file_max;
	newfs_super_d.root_ino = super.root_ino;
	ret = your_write(0, &newfs_super_d, sizeof(struct newfs_super_d));
	if (ret < 0) {
        NEWFS_DBG("[%s] newfs_destroy: write super block failed\n", __func__);
    }

	/* 4）关闭设备 */
	ddriver_close(super.fd);

	/* 5）释放内存 */
	if (super.ino_bitmap) {
		free(super.ino_bitmap);
		super.ino_bitmap = NULL;
	}
	if (super.data_bitmap) {
		free(super.data_bitmap);
		super.data_bitmap = NULL;
	}
	super.is_mounted = false;
	super.root_dentry = NULL;
	super.fd = -1;

	return;
}

/**
 * @brief 创建目录
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建模式（只读？只写？），可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_mkdir(const char* path, mode_t mode) {
	/* TODO: 解析路径，创建目录 */
	// step 1: 解析路径，找到父目录的inode
	bool is_find, is_root;
	char* fname;
	struct newfs_dentry* last_dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* dentry;
	struct newfs_inode*  inode;

	if (is_find) {
		return -NEWFS_ERROR_EXISTS;
	}

	if (NEWFS_IS_REG(last_dentry->inode)) {
		return -NEWFS_ERROR_UNSUPPORTED;
	}

	fname  = newfs_get_fname(path);
	// step 2: 创建新的目录项dentry，并添加到父目录中
	dentry = new_dentry(fname, NEWFS_DIR); 
	dentry->parent = last_dentry;
	// step 3: 分配新的索引节点inode
	inode = newfs_alloc_inode(dentry);
	newfs_alloc_dentry(last_dentry->inode, dentry);

	// 使用 inode 变量：同步到磁盘
    if (inode != NULL) {
        newfs_sync_inode(inode);           // 同步新创建的目录inode
        newfs_sync_inode(last_dentry->inode); // 同步父目录inode（因为添加了新的dentry）
    }

	return NEWFS_ERROR_NONE;
}

/**
 * @brief 获取文件或目录的属性，该函数非常重要
 * 
 * @param path 相对于挂载点的路径
 * @param newfs_stat 返回状态
 * @return int 0成功，否则返回对应错误号
 */
int newfs_getattr(const char* path, struct stat * newfs_stat) {
	/* TODO: 解析路径，获取Inode，填充newfs_stat，可参考/fs/simplefs/sfs.c的sfs_getattr()函数实现 */
	bool is_find, is_root;
	struct newfs_dentry* dentry = newfs_lookup(path, &is_find, &is_root);
	if (is_find == false) {
		return -NEWFS_ERROR_NOTFOUND;
	}

	if (NEWFS_IS_DIR(dentry->inode)) {
		newfs_stat->st_mode = S_IFDIR | NEWFS_DEFAULT_PERM;
		newfs_stat->st_size = dentry->inode->dir_cnt * sizeof(struct newfs_dentry_d);
	}
	else if (NEWFS_IS_REG(dentry->inode)) {
		newfs_stat->st_mode = S_IFREG | NEWFS_DEFAULT_PERM;
		newfs_stat->st_size = dentry->inode->size;
	}
	// else if (NEWFS_IS_SYM_LINK(dentry->inode)) {
	// 	newfs_stat->st_mode = S_IFLNK | NEWFS_DEFAULT_PERM;
	// 	newfs_stat->st_size = dentry->inode->size;
	// }

	newfs_stat->st_nlink = 1;
	newfs_stat->st_uid 	 = getuid();
	newfs_stat->st_gid 	 = getgid();
	newfs_stat->st_atime   = time(NULL);
	newfs_stat->st_mtime   = time(NULL);
	newfs_stat->st_blksize = NEWFS_IO_SZ();
	newfs_stat->st_blocks  = NEWFS_DATA_PER_FILE; /* 占用的逻辑块数 */

	if (is_root) {
		newfs_stat->st_size	= dentry->inode->dir_cnt * sizeof(struct newfs_dentry_d);/* 根目录大小为所有目录项之和 */
		newfs_stat->st_blocks = NEWFS_DISK_SZ() / NEWFS_IO_SZ();
		newfs_stat->st_nlink  = 2;		/* !特殊，根目录link数为2 */
	}
	return NEWFS_ERROR_NONE;
}

/**
 * @brief 遍历目录项，填充至buf，并交给FUSE输出
 * 
 * @param path 相对于挂载点的路径
 * @param buf 输出buffer
 * @param filler 参数讲解:
 * 
 * typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
 *				const struct stat *stbuf, off_t off)
 * buf: name会被复制到buf中
 * name: dentry名字
 * stbuf: 文件状态，可忽略
 * off: 下一次offset从哪里开始，这里可以理解为第几个dentry
 * 
 * @param offset 第几个目录项？
 * @param fi 可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset,
			    		 struct fuse_file_info * fi) {
    /* TODO: 解析路径，获取目录的Inode，并读取目录项，利用filler填充到buf，可参考/fs/simplefs/sfs.c的sfs_readdir()函数实现 */
	bool  	is_find, is_root;
	int		cur_dir = offset;

	struct newfs_dentry* dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* sub_dentry;
	struct newfs_inode* inode;
	if (is_find) {
		inode = dentry->inode;
		sub_dentry = newfs_get_dentry(inode, cur_dir);
		if (sub_dentry) {
			filler(buf, sub_dentry->name, NULL, ++offset);
		}
		return NEWFS_ERROR_NONE;
	}
	NEWFS_DBG("[%s] readdir: path not found %s\n", __func__, path);
	return -NEWFS_ERROR_NOTFOUND;
}


/**
 * @brief 创建文件
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建文件的模式，可忽略
 * @param dev 设备类型，可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_mknod(const char* path, mode_t mode, dev_t dev) {
	/* TODO: 解析路径，并创建相应的文件 */
	// step 1: 解析路径，找到父目录的inode
	bool is_find, is_root;
	char* fname;
	struct newfs_dentry* last_dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* dentry;
	struct newfs_inode*  inode;

	if (is_find) {
		return -NEWFS_ERROR_EXISTS;
	}

	if (NEWFS_IS_REG(last_dentry->inode)) {
		return -NEWFS_ERROR_UNSUPPORTED;
	}

	fname  = newfs_get_fname(path);
	// step 2: 创建新的目录项dentry，并添加到父目录中
	dentry = new_dentry(fname, NEWFS_REG_FILE); 
	dentry->parent = last_dentry;
	// step 3: 分配新的索引节点inode
	inode  = newfs_alloc_inode(dentry);
	newfs_alloc_dentry(last_dentry->inode, dentry);

	// 使用 inode 变量：同步到磁盘
	if (inode != NULL) {
		newfs_sync_inode(inode);           // 同步新创建的文件inode
		newfs_sync_inode(last_dentry->inode); // 同步父目录inode（因为添加了新的dentry）
	}
	
	return NEWFS_ERROR_NONE;
}

/**
 * @brief 修改时间，为了不让touch报错 
 * 
 * @param path 相对于挂载点的路径
 * @param tv 实践
 * @return int 0成功，否则返回对应错误号
 */
int newfs_utimens(const char* path, const struct timespec tv[2]) {
	(void)path;
	return 0;
}
/******************************************************************************
* SECTION: 选做函数实现
*******************************************************************************/
/**
 * @brief 写入文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 写入大小
 */
int newfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
	/* 选做 */
	return size;
}

/**
 * @brief 读取文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 读取大小
 */
int newfs_read(const char* path, char* buf, size_t size, off_t offset,
		      struct fuse_file_info* fi) {
	/* 选做 */
	return size;			   
}

/**
 * @brief 删除文件
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_unlink(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 删除目录
 * 
 * 一个可能的删除目录操作如下：
 * rm ./tests/mnt/j/ -r
 *  1) Step 1. rm ./tests/mnt/j/j
 *  2) Step 2. rm ./tests/mnt/j
 * 即，先删除最深层的文件，再删除目录文件本身
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rmdir(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 重命名文件 
 * 
 * @param from 源文件路径
 * @param to 目标文件路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rename(const char* from, const char* to) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开文件，可以在这里维护fi的信息，例如，fi->fh可以理解为一个64位指针，可以把自己想保存的数据结构
 * 保存在fh中
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_open(const char* path, struct fuse_file_info* fi) {
	/* 选做 */

	return 0;
}

/**
 * @brief 打开目录文件
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_opendir(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 改变文件大小
 * 
 * @param path 相对于挂载点的路径
 * @param offset 改变后文件大小
 * @return int 0成功，否则返回对应错误号
 */
int newfs_truncate(const char* path, off_t offset) {
	/* 选做 */
	// bool   is_find, is_root;
	// struct newfs_dentry* dentry = newfs_lookup(path, &is_find, &is_root);
	// struct newfs_inode*  inode;
	
	// if (is_find == false) {
	// 	return -NEWFS_ERROR_NOTFOUND;
	// }
	
	// inode = dentry->inode;

	// if (NEWFS_IS_DIR(inode)) {
	// 	return -NEWFS_ERROR_ISDIR;
	// }

	// if(offset > super.file_max) {
	// 	return -NEWFS_ERROR_NOSPACE;
	// }

	// inode->size = offset;

	return NEWFS_ERROR_NONE;
}


/**
 * @brief 访问文件，因为读写文件时需要查看权限
 * 
 * @param path 相对于挂载点的路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 0成功，否则返回对应错误号
 */
int newfs_access(const char* path, int type) {
	/* 选做: 解析路径，判断是否存在 */
	return 0;
}

/******************************************************************************
* SECTION: FUSE入口
*******************************************************************************/
int main(int argc, char **argv)
{
    int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	newfs_options.device = strdup("/home/students/2023311819/user-land-filesystem/driver/user_ddriver/bin/ddriver");

	if (fuse_opt_parse(&args, &newfs_options, option_spec, NULL) == -1)
		return -1;
	
	ret = fuse_main(args.argc, args.argv, &operations, NULL);
	fuse_opt_free_args(&args);
	return ret;
}