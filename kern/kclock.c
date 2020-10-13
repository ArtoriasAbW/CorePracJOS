/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <kern/kclock.h>

void
rtc_init(void) {
  nmi_disable();
  // LAB 4: Your code here
  outb(IO_RTC_CMND, RTC_AREG); // переключаемся на A
  uint8_t value = inb(IO_RTC_DATA); // читаем
  outb(IO_RTC_CMND, RTC_AREG);
  outb(IO_RTC_DATA, SET_NEW_RATE(value, RTC_500MS_RATE)); // 500ms частота
  outb(IO_RTC_CMND, RTC_BREG); // переключаемся на B
  value = inb(IO_RTC_DATA); // читаем значение
  outb(IO_RTC_CMND, RTC_BREG); // переключаемся на B
  outb(IO_RTC_DATA, value | RTC_PIE); // установливаем PIE bit

}

uint8_t
rtc_check_status(void) {
  uint8_t status = 0;
  // LAB 4: Your code here
  outb(IO_RTC_CMND, RTC_CREG); // переключаемся на C
  status = inb(IO_RTC_DATA); // читаем значение
  return status;
}
