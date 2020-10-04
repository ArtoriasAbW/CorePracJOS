/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <kern/kclock.h>

void
rtc_init(void) {
  nmi_disable();
  // LAB 4: Your code here
  outb(IO_RTC_CMND, 0x8A); // переключаемся на A
  uint8_t a_value = inb(IO_RTC_DATA); // читаем
  outb(IO_RTC_CMND, 0x8A);
  outb(IO_RTC_DATA, a_value | 011); // 500ms частота
  outb(IO_RTC_CMND, 0x8B); // переключаемся на B
  uint8_t b_value = inb(IO_RTC_DATA); // читаем значение
  outb(IO_RTC_CMND, 0x8B); // переключаемся на B
  outb(IO_RTC_DATA, b_value | 0x40); // установливаем PIE bit

}

uint8_t
rtc_check_status(void) {
  uint8_t status = 0;
  // LAB 4: Your code here
  outb(IO_RTC_CMND, 0x0C); // переключаемся на C
  status = inb(IO_RTC_DATA); // читаем значение
  return status;
}
