#pragma once

// LCD + touch interface (LCD_UI in config.h, Freenove FNK0104S): status bar,
// spectrum + waterfall (tap sets the CW tone, long press back to auto),
// decoded CW/RTTY text, FT8/FT4 list, FAX/SSTV image and settings. Reads
// everything from ui_hub; runs in the LVGL task on core 0.

// Starts the display, the touch controller and the LVGL task. No-op when
// LCD_UI is 0.
void lcd_ui_start();
