/* drivers/serial.c —— COM1 单向输出（printk 的第二条腿）
 *
 * 寄存器偏移（相对 0x3F8）：
 *   0 = 发送/接收数据（DLAB=1 时是分频低字节）
 *   1 = 中断使能（DLAB=1 时是分频高字节）
 *   2 = FIFO 控制
 *   3 = 线路控制（bit7 = DLAB）
 *   4 = Modem 控制
 *   5 = 线路状态（bit5 = 发送保持寄存器空）
 * 分频 = 115200 / 目标波特率，这里取 1 = 115200。
 */
#include "linux/serial.h"
#include "asm/io.h"

#define REG_DATA   (SERIAL_COM1 + 0)
#define REG_IER    (SERIAL_COM1 + 1)
#define REG_FCR    (SERIAL_COM1 + 2)
#define REG_LCR    (SERIAL_COM1 + 3)
#define REG_MCR    (SERIAL_COM1 + 4)
#define REG_LSR    (SERIAL_COM1 + 5)

#define LSR_THR_EMPTY 0x20

static int ser_on;

void serial_init(void)
{
    outb(REG_IER, 0x00);            /* 关掉全部串口中断：我们只用轮询发送 */
    outb(REG_LCR, 0x80);            /* DLAB=1，接下来两个字节是分频值 */
    outb(REG_DATA, 0x01);           /* 分频低字节：1 → 115200 */
    outb(REG_IER, 0x00);            /* 分频高字节 */
    outb(REG_LCR, 0x03);            /* 8 数据位、无校验、1 停止位，DLAB=0 */
    outb(REG_FCR, 0xC7);            /* 使能+清空 FIFO，14 字节触发 */
    outb(REG_MCR, 0x0B);            /* DTR/RTS/OUT2：让对端看到"我在线" */
    ser_on = 1;
}

int serial_ready(void)
{
    return ser_on;
}

/* 等发送保持寄存器空。给自旋上限：设备不在时宁可丢字，也不能把内核卡死
 * （这台虚拟机里串口永远在，但真机上少一根线就会挂）。 */
static int serial_wait(void)
{
    unsigned long spin = 0;

    while ((inb(REG_LSR) & LSR_THR_EMPTY) == 0) {
        if (++spin > 200000UL)
            return 0;
    }
    return 1;
}

void serial_putc(char c)
{
    if (!ser_on || !serial_wait())
        return;
    outb(REG_DATA, (unsigned char)c);
}

void serial_puts(const char *s)
{
    while (s && *s)
        serial_putc(*s++);
}
