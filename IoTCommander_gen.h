/**
 * @file IoTCommander_gen.h
 */

#ifndef LVGL_PRO_IOTCOMMANDER_GEN_H
#define LVGL_PRO_IOTCOMMANDER_GEN_H

#ifndef UI_SUBJECT_STRING_LENGTH
#define UI_SUBJECT_STRING_LENGTH 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
    #include "lvgl_private.h"
#else
    #include "lvgl/lvgl.h"
    #include "lvgl/lvgl_private.h"
#endif

#if defined(LV_USE_XML) && LV_USE_XML
    #include "lv_xml/lv_xml.h"
#endif



/* Prototypes for target functions, needed by responsive const definitions */

void IoTCommander_set_target(uint32_t target);
uint32_t IoTCommander_get_target(void);
bool IoTCommander_check_target(uint32_t target);

/*********************
 *      DEFINES
 *********************/

#define IOTCOMMANDER_TARGET_UNDEFINED           (0 << 1)
#define IOTCOMMANDER_TARGET_VIEWESMART_240X320  (1 << 1)
#define IOTCOMMANDER_TARGET_ALL                 0x0FFFFFFF

/* By default compile for all targets, allowing to switch to any targets at runtime */
#ifndef IOTCOMMANDER_COMPILE_TARGET
#define IOTCOMMANDER_COMPILE_TARGET IOTCOMMANDER_TARGET_ALL
#endif

#define IOTCOMMANDER_CHECK_COMPILE_TARGET(target) (IOTCOMMANDER_COMPILE_TARGET & (target) ? 1 : 0)

#ifndef LV_XML_EVAL_STRING_BUF_SIZE
    #define LV_XML_EVAL_STRING_BUF_SIZE 256
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL VARIABLES
 **********************/

/*-------------------
 * Permanent screens
 *------------------*/

/*----------------
 * Global styles
 *----------------*/

extern lv_style_t iotc_background;
extern lv_style_t iotc_panel;
extern lv_style_t iotc_accent;

/*----------------
 * Fonts
 *----------------*/



/*----------------
 * Images
 *----------------*/



/*----------------
 * Subjects
 *----------------*/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/*----------------
 * Event Callbacks
 *----------------*/

/**
 * Initialize the component library
 */

void IoTCommander_init_gen(const char * asset_path);

/**********************
 *      MACROS
 **********************/

/**********************
 *   POST INCLUDES
 **********************/

/*Include all the widgets, components and screens of this library*/
#include "screens/Alerts_gen.h"
#include "screens/Dashboard_gen.h"
#include "screens/Devices_gen.h"
#include "screens/Settings_gen.h"

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LVGL_PRO_IOTCOMMANDER_GEN_H*/