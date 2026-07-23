char md5[] = "b1a8c0845d49d1bc43ac3b252b41788d";
/*
 * This file is part of OpenPLC Runtime
 *
 * Copyright (C) 2023 Autonomy, GP Orcullo
 * Based on the work by GP Orcullo on Beremiz for uC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <stdio.h>

#define SAME_ENDIANNESS      0
#define REVERSE_ENDIANNESS   1

uint8_t endianness;

#define VAR_COUNT               0

uint16_t get_var_count(void)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0xE90F36DAU */
    CFI_PROLOGUE_TAG(0xE90F36DAU);
    #endif

    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0xE90F36DAU);
    #endif
    return VAR_COUNT;
}

size_t get_var_size(size_t idx)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0xACD3063DU */
    CFI_PROLOGUE_TAG(0xACD3063DU);
    #endif

    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0xACD3063DU);
    #endif
    return 0;
}

void *get_var_addr(size_t idx)
{
    return 0;
}

void force_var(size_t idx, bool forced, void *val)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0xAAD70696U */
    CFI_PROLOGUE_TAG(0xAAD70696U);
    #endif

    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0xAAD70696U);
    #endif
}

void swap_bytes(void *ptr, size_t size) 
{
    #if CFI_PROTECT
    /* CFI: default, tag=0xC0F3B04CU */
    CFI_PROLOGUE_TAG(0xC0F3B04CU);
    #endif

    uint8_t *bytePtr = (uint8_t *)ptr;
    size_t i;
    for (i = 0; i < size / 2; ++i) 
    {
        uint8_t temp = bytePtr[i];
        bytePtr[i] = bytePtr[size - 1 - i];
        bytePtr[size - 1 - i] = temp;
    }
    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0xC0F3B04CU);
    #endif
}

void trace_reset(void)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0x95C29AA6U */
    CFI_PROLOGUE_TAG(0x95C29AA6U);
    #endif

    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0x95C29AA6U);
    #endif
}

void set_trace(size_t idx, bool forced, void *val)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0x729F25F7U */
    CFI_PROLOGUE_TAG(0x729F25F7U);
    #endif

    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0x729F25F7U);
    #endif
}

void set_endianness(uint8_t value)
{
    #if CFI_PROTECT
    /* CFI: default, tag=0x9C80A1CFU */
    CFI_PROLOGUE_TAG(0x9C80A1CFU);
    #endif

    if (value == SAME_ENDIANNESS || value == REVERSE_ENDIANNESS)
    {
        endianness = value;
    }
    #if CFI_PROTECT
    CFI_EPILOGUE_TAG(0x9C80A1CFU);
    #endif
}
