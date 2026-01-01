/* 标准输出和报错机制 */

#include "mod.h"

static char digits[] = "0123456789abcdef";

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    cons_init();
    spinlock_init(&print_lk, "printf");
}

/* %d %x */
static void printint(int xx, int base, int sign)
{
    char buf[16];
    int i;
    uint32 x;

    if (sign && (sign = xx < 0))
        x = -xx;
    else
        x = xx;

    i = 0;
    do
    {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
        buf[i++] = '-';

    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

/* %p */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

/*
    标准化输出, 需要支持:
    1. %d (32位有符号数,以10进制输出)
    2. %x (32位无符号数,以16进制输出)
    3. %p (64位无符号数,以0x开头的16进制输出)
    4. %c (单个字符)
    5. %s (字符串)
    提示: stdarg.h中的va_list中包括你需要的参数地址
*/
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, c;
    const char *s;
    int locking = 0;

    if (fmt == 0)
        panic("printf: null format string");

    // 检查是否已经持有锁
    // [修复] print_lock -> print_lk
    if (!spinlock_holding(&print_lk)) {
        spinlock_acquire(&print_lk);
        locking = 1;
    }

    va_start(ap, fmt);
    for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
        if (c != '%') {
            uart_putc_sync(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if (c == 0)
            break;
        switch (c) {
        case 'd':
            // [修复] print_integer -> printint
            printint(va_arg(ap, int), 10, 1);
            break;
        case 'x':
            // [修复] print_integer -> printint
            printint(va_arg(ap, uint32), 16, 0);
            break;
        case 'p':
            // [修复] print_pointer -> printptr
            printptr(va_arg(ap, uint64));
            break;
        case 'c':
            uart_putc_sync(va_arg(ap, int));
            break;
        case 's':
            if ((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for (; *s; s++)
                uart_putc_sync(*s);
            break;
        case '%':
            uart_putc_sync('%');
            break;
        default:
            // 未知格式，原样打印
            uart_putc_sync('%');
            uart_putc_sync(c);
            break;
        }
    }
    va_end(ap);

    // [修复] print_lock -> print_lk
    if (locking)
        spinlock_release(&print_lk);
}

/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* 报错并终止输出 */
void panic(const char *s)
{
    printf("panic! %s\n", s);
    panicked = 1;
    while (1)
        ;
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition) {
        panic(warning);
    }
}