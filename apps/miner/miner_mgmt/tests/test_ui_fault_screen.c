// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ui/ui_screen.c"

static void render(uint16_t id)
{
    ui_screen_render(id);
    for (int i = 0; i < 30; i++) {
        lv_tick_inc(100);
        lv_timer_handler();
    }
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(LCD_HOR_RES, LCD_VER_RES);
    assert(display);
    g_screens[SC_BLACK]     = make_screen();
    g_screens[SC_ERRORCODE] = screen_errorcode("");
    g_screens[SC_FAULT]     = screen_fault();

    static const struct {
        uint16_t    id;
        const char *code;
        const char *desc;
    } faults[] = {
        { UI_HASH_ERR, "E-06", "Hashboard Fault" },
        { UI_FAN_STALL, "E-07", "Fan Stopped" },
        { UI_AGING_ERR, "E-08", "Aging Failed" },
        { UI_POWER_FAULT, "E-09", "Power Fault" },
    };
    for (unsigned i = 0; i < sizeof faults / sizeof faults[0]; i++) {
        render(faults[i].id);
        assert(lv_screen_active() == g_screens[SC_FAULT]);
        assert(!strcmp(lv_label_get_text(g_fault_code), faults[i].code));
        assert(!strcmp(lv_label_get_text(g_fault_desc), faults[i].desc));
    }
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_screens[SC_FAULT], 0)), LV_SYMBOL_WARNING));

    render(UI_FAN_ERR);
    assert(lv_screen_active() == g_screens[SC_ERRORCODE]);
    assert(!strcmp(lv_label_get_text(g_errcode_label), "E-04"));
    assert(id_to_slot(UI_CHECK_POOL) == SC_CHECK_POOL);

    lv_obj_t *info     = make_screen();
    g_info.ip_label    = lv_label_create(info);
    g_info.hash_label  = lv_label_create(info);
    g_info.mode_label  = lv_label_create(info);
    g_info.power_label = lv_label_create(info);
    g_info.hash_arc    = make_arc(info, lv_color_hex(0xe2b020), 217);
    g_info.power_arc   = make_arc(info, lv_color_hex(0x24b7b1), 219);
    ui_summary fresh   = { "192.168.1.2", "1.23", "High", "140", 3500, 140,
          UI_RDY_HASH | UI_RDY_POWER, 0 };
    ui_screen_refresh(&fresh);
    render(UI_OFF);
    assert(!strcmp(lv_label_get_text(g_info.ip_label), "192.168.1.2"));
    assert(lv_arc_get_value(g_info.hash_arc) == 50 && lv_arc_get_value(g_info.power_arc) == 100);
    ui_summary stale = { "--", "--", "--", "--", 0, 0, 0, 0 };
    ui_screen_refresh(&stale);
    render(UI_OFF);
    assert(lv_arc_get_value(g_info.hash_arc) == 0 && lv_arc_get_value(g_info.power_arc) == 0);
    assert(!strcmp(lv_label_get_text(g_info.ip_label), "--"));
    assert(!strcmp(lv_label_get_text(g_info.hash_label), "--"));
    assert(!strcmp(lv_label_get_text(g_info.mode_label), "--"));
    assert(!strcmp(lv_label_get_text(g_info.power_label), "--"));

    lv_display_delete(display);
    lv_deinit();
    puts("fault code screens OK");
    return 0;
}
