/**
 * @file Alerts_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "Alerts_gen.h"
#include "../IoTCommander.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/***********************
 *  STATIC VARIABLES
 **********************/

/***********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * Alerts_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if IOTCOMMANDER_CHECK_COMPILE_TARGET(IOTCOMMANDER_TARGET_ALL)
    if (IoTCommander_check_target(IOTCOMMANDER_TARGET_ALL)) {
        lv_obj_t * lv_obj_0 = lv_obj_create(NULL);
        lv_obj_set_name_static(lv_obj_0, "Alerts_#");
        lv_obj_set_width(lv_obj_0, 240);
        lv_obj_set_height(lv_obj_0, 320);
        lv_obj_set_style_bg_color(lv_obj_0, lv_color_hex(0x0B1220), 0);

        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_obj_set_x(lv_label_0, 12);
        lv_obj_set_y(lv_label_0, 12);
        lv_label_set_text(lv_label_0, "ALERTS");
        lv_obj_set_style_text_color(lv_label_0, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_obj_set_x(lv_label_1, 12);
        lv_obj_set_y(lv_label_1, 38);
        lv_label_set_text(lv_label_1, "Events and notifications");
        lv_obj_set_style_text_color(lv_label_1, lv_color_hex(0x18C29C), 0);

        lv_obj_t * lv_obj_1 = lv_obj_create(lv_obj_0);
        lv_obj_set_x(lv_obj_1, 12);
        lv_obj_set_y(lv_obj_1, 72);
        lv_obj_set_width(lv_obj_1, 216);
        lv_obj_set_height(lv_obj_1, 76);
        lv_obj_set_style_bg_color(lv_obj_1, lv_color_hex(0x172235), 0);
        lv_obj_set_style_radius(lv_obj_1, 10, 0);
        lv_obj_t * lv_label_2 = lv_label_create(lv_obj_1);
        lv_obj_set_x(lv_label_2, 12);
        lv_obj_set_y(lv_label_2, 12);
        lv_label_set_text(lv_label_2, "SYSTEM STATUS");
        lv_obj_set_style_text_color(lv_label_2, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t * lv_label_3 = lv_label_create(lv_obj_1);
        lv_obj_set_x(lv_label_3, 12);
        lv_obj_set_y(lv_label_3, 38);
        lv_label_set_text(lv_label_3, "Awaiting events");
        lv_obj_set_style_text_color(lv_label_3, lv_color_hex(0xA8B7CC), 0);

        lv_obj_t * lv_label_4 = lv_label_create(lv_obj_0);
        lv_obj_set_x(lv_label_4, 12);
        lv_obj_set_y(lv_label_4, 175);
        lv_label_set_text(lv_label_4, "No live alert feed yet");
        lv_obj_set_style_text_color(lv_label_4, lv_color_hex(0xF4C95D), 0);

        lv_obj_t * lv_label_5 = lv_label_create(lv_obj_0);
        lv_obj_set_x(lv_label_5, 12);
        lv_obj_set_y(lv_label_5, 288);
        lv_label_set_text(lv_label_5, "IoT Commander");
        lv_obj_set_style_text_color(lv_label_5, lv_color_hex(0xA8B7CC), 0);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
