idf_component_register(
    SRCS "main.c" "display_bsp.c"
    INCLUDE_DIRS "."
    REQUIRES touch_bsp read_lcd_id_bsp fatfs)
