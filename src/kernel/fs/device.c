#include "mod.h"

// 外部库函数声明 (通常由 console.c, uart.c 或其他 lib 提供)
extern uint32 cons_read(uint64 dst, uint32 len, bool is_user);
extern uint32 cons_write(uint64 src, uint32 len, bool is_user);
extern void   cons_putc(char c);
extern char   cons_getc(void);
extern uint32 gpt_query(uint64 src, uint32 len, bool is_user);

/**
 * 设备初始化
 * 确保 /dev 目录下的设备文件在磁盘上存在
 * 这样用户程序才能通过 open("/dev/stdin", ...) 访问它们
 */
void device_init() {
    // 检查 /dev 目录，如果不存在 path_create_inode 会根据路径递归或报错
    // 实际上 mkfs.c 在制作镜像时已经预置了这些 inode
    // 这里主要确保内核对这些设备号有认知
}

/**
 * 检查设备打开权限的合法性
 */
bool device_open_check(uint16 major, uint32 open_mode) {
    switch (major) {
        case DEV_STDIN:
            // 标准输入只能读
            return (open_mode == O_RDONLY);
        case DEV_STDOUT:
        case DEV_STDERR:
        case DEV_GPT:
            // 标准输出、错误输出、GPT 只能写
            return (open_mode == O_WRONLY);
        case DEV_ZERO:
        case DEV_NULL:
            // 零文件和黑洞文件读写均可
            return true;
        default:
            return false;
    }
}

/**
 * 设备读接口：将不同主设备号的操作重定向到具体的驱动函数
 */
uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst) {
    uint32 i;
    char zero = 0;

    switch (major) {
        case DEV_STDIN:
            // 从行缓冲控制台读取输入
            return cons_read(dst, len, is_user_dst);

        case DEV_ZERO:
            // /dev/zero: 读到的永远是 0
            for (i = 0; i < len; i++) {
                if (either_copy_to(is_user_dst, dst + i, &zero, 1) < 0)
                    return i;
            }
            return len;

        case DEV_NULL:
            // /dev/null: 读到的字节数永远是 0
            return 0;

        default:
            return 0;
    }
}

/**
 * 设备写接口：将不同主设备号的操作重定向到具体的驱动函数
 */
uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src) {
    char c;
    uint32 i;

    switch (major) {
        case DEV_STDOUT:
            // 标准输出：直接打印到控制台
            return cons_write(src, len, is_user_src);

        case DEV_STDERR:
            // 标准错误：增加 "ERROR: " 前缀后输出
            printf("ERROR: ");
            return cons_write(src, len, is_user_src);

        case DEV_NULL:
            // /dev/null: 黑洞，写入多少都认为成功，但不产生任何效果
            return len;

        case DEV_GPT:
            // 小彩蛋：GPT 逻辑
            return gpt_query(src, len, is_user_src);

        default:
            return 0;
    }
}

// ------------------------------------------------------------------
// 以下是简单的辅助函数，如果你的 console.c 已经实现了 cons_read/write，
// 则 device_read_data 直接调用它们即可。
// ------------------------------------------------------------------

/**
 * 示例：简单的 stdin 读取实现 (如果没有 cons_read)
 */
uint32 stdin_read(uint32 len, uint64 dst, bool is_user) {
    return cons_read(dst, len, is_user);
}

/**
 * 示例：简单的 stdout 写入实现 (如果没有 cons_write)
 */
uint32 stdout_write(uint32 len, uint64 src, bool is_user) {
    char c;
    for (uint32 i = 0; i < len; i++) {
        if (either_copy_from(is_user, &c, src + i, 1) < 0)
            return i;
        cons_putc(c);
    }
    return len;
}