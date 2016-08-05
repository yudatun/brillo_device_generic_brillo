/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bub_sysdeps.h"
#include <stdio.h>

int utf8_to_ucs2(const uint8_t* utf8_data,
                 size_t utf8_num_bytes,
                 uint16_t* ucs2_data,
                 size_t ucs2_data_capacity_num_bytes) {
  uint32_t i8 = 0;
  uint32_t i2 = 0;
  do {
    // cannot represent this character, too long
    if ((utf8_data[i8] & 0xF8) == 0xF0)
      return 1;
    else if ((utf8_data[i8] & 0xF0) == 0xE0) {
      ucs2_data[i2] = (uint16_t)((uint16_t)utf8_data[i8] << 12) |
                      (uint16_t)(((uint16_t)utf8_data[i8 + 1] << 6) & 0x0FC0) |
                      (uint16_t)((uint16_t)utf8_data[i8 + 2 ] & 0x003F);
      i8 += 3;
    }
    else if ((utf8_data[i8] & 0xE0) == 0XC0) {
      ucs2_data[i2] = (uint16_t)(((uint16_t)utf8_data[i8] << 6) & 0x07C0) |
                      (uint16_t)((uint16_t)utf8_data[i8 + 1] & 0x003F);
      i8 += 2;
    }
    else if (!(utf8_data[i8] >> 7)) {
      ucs2_data[i2] = (uint16_t)((uint16_t)utf8_data[i8] & 0x00FF);
      i8++;
    }
    // invalid utf-8
    else
      return 1;
    i2++;
  } while (i8 < utf8_num_bytes - 1);
  return 0;
}

void* bub_calloc(size_t size) {
  void *x = bub_malloc_(size);

  if (x != NULL)
    bub_memset(x, 0, size);

  return x;
}