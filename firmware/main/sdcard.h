#ifndef AGR_SDCARD_H
#define AGR_SDCARD_H

#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sdmmc_cmd.h"
#include "diskio.h"

bool Sdcard_Setup();

#endif