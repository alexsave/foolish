// The mixed-radix arithmetic. See mixrad.h.
#include "mixrad.h"

int mixrad_mul_add(uint8_t *v, int *len, int cap, uint32_t base, uint32_t digit)
{
    uint32_t carry = digit;
    for (int i = 0; i < *len; i++) {
        uint32_t t = (uint32_t)v[i] * base + carry;
        v[i] = (uint8_t)(t & 0xff);
        carry = t >> 8;
    }
    while (carry) {
        if (*len >= cap) return 0;
        v[(*len)++] = (uint8_t)(carry & 0xff);
        carry >>= 8;
    }
    return 1;
}

uint32_t mixrad_div_mod(uint8_t *v, int *len, uint32_t base)
{
    uint32_t rem = 0;
    for (int i = *len - 1; i >= 0; i--) {
        uint32_t cur = (rem << 8) | v[i];
        v[i] = (uint8_t)(cur / base);
        rem  = cur % base;
    }
    while (*len > 0 && v[*len - 1] == 0) (*len)--;
    return rem;
}
