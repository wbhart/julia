#ifndef IA_MISC_H
#define IA_MISC_H

#include <stdint.h>
#include <immintrin.h>

static inline uint64_t rdtsc()
{
    unsigned hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

#if (__MIC__)

static inline void cpu_pause()
{
    _mm_delay_64(100);
}

static inline void cpu_delay(int64_t cycles)
{
    _mm_delay_64(cycles);
}

static inline void cpu_mfence()
{
    __asm__ __volatile__ ("":::"memory");
}

static inline void cpu_sfence()
{
    __asm__ __volatile__ ("":::"memory");
}

static inline void cpu_lfence()
{
    __asm__ __volatile__ ("":::"memory");
}

#else  /* !__MIC__ */

static inline void cpu_pause()
{
    _mm_pause();
}

static inline void cpu_delay(int64_t cycles)
{
    uint64_t s = rdtsc();
    while ((rdtsc() - s) < cycles)
        _mm_pause();
}

static inline void cpu_mfence()
{
    _mm_mfence();
}

static inline void cpu_sfence()
{
    _mm_sfence();
}

static inline void cpu_lfence()
{
    _mm_lfence();
}

#endif /* __MIC__ */


#endif  /* IA_MISC_H */

