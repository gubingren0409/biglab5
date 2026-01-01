#include "mod.h"
#include "type.h"          // [修复] 必须包含，定义了 device_t, INODE_MAJOR_*
#include "../proc/mod.h"   // 引入 myproc
#include "../mem/method.h" // 引入 pmem_alloc, uvm_copy, pmem_stat
#include "../lib/method.h" // 引入 cons_read, cons_write, printf, memmove

// 1. 补充缺失的宏定义，映射到 type.h 中的常量
#define DEV_STDIN     INODE_MAJOR_STDIN
#define DEV_STDOUT    INODE_MAJOR_STDOUT
#define DEV_STDERR    INODE_MAJOR_STDERR
#define DEV_ZERO      INODE_MAJOR_ZERO
#define DEV_NULL      INODE_MAJOR_NULL
#define DEV_GPT       INODE_MAJOR_GPT0

// 映射 Open Mode
#define O_RDONLY      FILE_OPEN_READ
#define O_WRONLY      FILE_OPEN_WRITE
#define O_RDWR        (FILE_OPEN_READ | FILE_OPEN_WRITE)

// 2. 实现辅助拷贝函数 (either_copy)
static int either_copy_to(bool is_user, uint64 dst, void *src, uint32 len) {
    if (is_user) {
        uvm_copyout(myproc()->pgtbl, dst, (uint64)src, len);
    } else {
        memmove((void*)dst, src, len);
    }
    return 0;
}

static int either_copy_from(bool is_user, void *dst, uint64 src, uint32 len) {
    if (is_user) {
        uvm_copyin(myproc()->pgtbl, (uint64)dst, src, len);
    } else {
        memmove(dst, (void*)src, len);
    }
    return 0;
}

device_t device_table[N_DEVICE];

/* 标准输入设备 */
static uint32 device_stdin_read(uint32 len, uint64 dst, bool is_user_dst)
{
    // 调用 lib/console.c 中的 cons_read (注意参数顺序: len, dst, flag)
    return cons_read(len, dst, is_user_dst);
}

/* 标准输出设备 */
static uint32 device_stdout_write(uint32 len, uint64 src, bool is_user_src)
{
    return cons_write(len, src, is_user_src);
}

/* 标准错误输出设备 */
static uint32 device_stderr_write(uint32 len, uint64 src, bool is_user_src)
{
    printf("ERROR: ");
    return cons_write(len, src, is_user_src);
}

/* 无限0流 (/dev/zero) */
static uint32 device_zero_read(uint32 len, uint64 dst, bool is_user_dst)
{
    uint32 write_len = 0, cut_len = 0;
    // 分配一个全0的物理页作为源
    uint64 src = (uint64)pmem_alloc(true); 
    if (!src) return 0;
    memset((void*)src, 0, PGSIZE);

    while (write_len < len)
    {
        cut_len = (len - write_len > PGSIZE) ? PGSIZE : (len - write_len);
        
        if (either_copy_to(is_user_dst, dst, (void*)src, cut_len) < 0) {
            break;
        }

        dst += cut_len;
        write_len += cut_len;
    }

    pmem_free(src, true);
    return write_len;
}

/* 空设备读取 (/dev/null) */
static uint32 device_null_read(uint32 len, uint64 dst, bool is_user_dst)
{
    return 0; // EOF
}

/* 空设备写入 (/dev/null) */
static uint32 device_null_write(uint32 len, uint64 src, bool is_user_src)
{
    return len; // 丢弃所有数据，假装写入成功
}

/* 彩蛋: 笨蛋GPT */
static uint32 device_gpt0_write(uint32 len, uint64 src, bool is_user_src)
{
    char tmp[MAXLEN_FILENAME + 1]; 
    uint32 copy_len = (len > MAXLEN_FILENAME) ? MAXLEN_FILENAME : len;
    proc_t *p = myproc();

    if (either_copy_from(is_user_src, tmp, src, copy_len) < 0)
        return 0;
    
    tmp[copy_len] = '\0';

    if (strncmp(tmp, "Hello", copy_len) == 0) {
        printf("Hi, I am gpt0!\n");
    } else if (strncmp(tmp, "Guess who I am", copy_len) == 0) {
        printf("Your procid is %d and name is %s.\n", p->pid, p->name);
    } else if (strncmp(tmp, "How many free memory left", copy_len) == 0) {
        uint32 kernel_free_pages, user_free_pages;
        pmem_stat(&kernel_free_pages, &user_free_pages);
        printf("We have %d free pages in kernel space, %d free pages in user space!\n",
            kernel_free_pages, user_free_pages);
    } else if (strncmp(tmp, "Good job", copy_len) == 0) {
        printf("Thanks for your kind words!\n");
    } else {
        printf("Sorry, I can not understand it.\n");
    }

    return len;
}

/* 注册设备 */
static void device_register(uint32 index, char* name,
    uint32(*read)(uint32, uint64, bool),
    uint32(*write)(uint32, uint64, bool))
{
    if (index >= N_DEVICE) return;
    memmove(device_table[index].name, name, MAXLEN_FILENAME);
    device_table[index].read = read;
    device_table[index].write = write;
}

/* 初始化device_table */
void device_init()
{
    // 清空表
    memset(device_table, 0, sizeof(device_table));

    // 注册所有设备
    device_register(DEV_STDIN,  "stdin",  device_stdin_read,  NULL);
    device_register(DEV_STDOUT, "stdout", NULL,               device_stdout_write);
    device_register(DEV_STDERR, "stderr", NULL,               device_stderr_write);
    device_register(DEV_ZERO,   "zero",   device_zero_read,   NULL);
    device_register(DEV_NULL,   "null",   device_null_read,   device_null_write);
    device_register(DEV_GPT,    "gpt0",   NULL,               device_gpt0_write);
}

/* 检查文件major字段的合法性及权限 */
bool device_open_check(uint16 major, uint32 open_mode)
{
    if (major >= N_DEVICE) return false;

    switch (major) {
        case DEV_STDIN:
            return (open_mode == O_RDONLY);
        case DEV_STDOUT:
        case DEV_STDERR:
        case DEV_GPT:
            return (open_mode == O_WRONLY);
        case DEV_ZERO:
            return (open_mode == O_RDONLY);
        case DEV_NULL:
            return true; // null 可读可写
        default:
            return false;
    }
}

/* 从设备文件中读取数据 */
uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst)
{
    if (major >= N_DEVICE || !device_table[major].read)
        return 0; // 不支持读取或设备不存在
    return device_table[major].read(len, dst, is_user_dst);
}

/* 向设备文件写入数据 */
uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src)
{
    if (major >= N_DEVICE || !device_table[major].write)
        return 0; // 不支持写入或设备不存在
    return device_table[major].write(len, src, is_user_src);
}