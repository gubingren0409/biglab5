#pragma once

/* 标准输出与错误处理函数 */

void print_init(void);
void printf(const char *fmt, ...);
void panic(const char *s);
void assert(bool condition, const char *warning);

/* UART驱动函数 */

void uart_init(void);
void uart_putc_sync(int c);
int uart_getc_sync(void);
void uart_intr(void);

/* 获得CPU信息 */

int mycpuid(void);
cpu_t *mycpu(void);