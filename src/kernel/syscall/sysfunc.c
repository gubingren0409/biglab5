#include "mod.h"

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
uint64 sys_brk()
{

}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{

}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{

}

/*
    打印一个字符串
    char *str
    成功返回0
*/
uint64 sys_print_str()
{

}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int()
{

}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork()
{

}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait()
{

}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit()
{

}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{

}

/*
    返回当前进程的pid
*/
uint64 sys_getpid()
{

}