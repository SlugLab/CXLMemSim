//
// Created by victoryang00 on 1/14/23.
//

#include "incore.h"

void pcm_cpuid(const unsigned leaf, CPUID_INFO* info)
{
    __asm__ __volatile__ ("cpuid" : \
                         "=a" (info->reg.eax),
                         "=b" (info->reg.ebx),
                         "=c" (info->reg.ecx),
                         "=d" (info->reg.edx) : "a" (leaf));
}

