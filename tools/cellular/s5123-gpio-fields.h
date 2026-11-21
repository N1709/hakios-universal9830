/* MIT. Pure GPIO field updates; no physical I/O. Preserve every other pin. */
#ifndef S5123_GPIO_FIELDS_H
#define S5123_GPIO_FIELDS_H
#include <stdint.h>
static inline int
s5123_gpio_output_values(uint32_t con, uint32_t dat, unsigned pin, unsigned level,
    uint32_t *new_con, uint32_t *new_dat)
{
    if (pin>7 || level>1 || !new_con || !new_dat) return 0;
    *new_dat=(dat & ~(1u<<pin)) | (level<<pin);
    *new_con=(con & ~(15u<<(4*pin))) | (1u<<(4*pin));
    return 1;
}
#endif
