#ifndef AGR_OPTIONS_H
#define AGR_OPTIONS_H
#include "esp_system.h"
#include "hal.h"

typedef enum {
    OPTION_TYPE_NUMERIC,
    OPTION_TYPE_BOOL,
    OPTION_TYPE_LOOPS,
    OPTION_TYPE_PLAYMODE,
    OPTION_TYPE_BACKLIGHTTIME,
    OPTION_TYPE_COUNT
} optiontype_t;

typedef enum {
    OPTION_CATEGORY_PLAYBACK,
    OPTION_CATEGORY_SCREEN,
    OPTION_CATEGORY_LEDS,
} optioncategory_t;

typedef struct {
    const char *name;
    const char *description;
    optioncategory_t category;
    optiontype_t type;
    //uint8_t width; //make them all 8bit lmaooooo
    volatile void *var;
    uint8_t defaultval;
} option_t;

#define OPTIONS_ACTUAL_VER 0x01

#if defined HWVER_PORTABLE
#define OPTIONS_VER OPTIONS_ACTUAL_VER
#elif defined HWVER_DESKTOP
#define OPTIONS_VER (0x80 | OPTIONS_ACTUAL_VER)
#else
#define OPTIONS_VER OPTIONS_ACTUAL_VER
#endif

static const option_t Options[];

extern volatile bool OptionsMgr_Unsaved;

void OptionsMgr_Setup();
void OptionsMgr_Main();
void OptionsMgr_Touch();

#endif