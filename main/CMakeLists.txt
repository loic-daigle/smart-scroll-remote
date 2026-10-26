idf_component_register(
    SRCS "main.c" "matter.cpp" "menu_system_matter.cpp" "src/button_menu.c"
    INCLUDE_DIRS "." "include"
    REQUIRES esp_matter esp_matter_console nvs_flash led_strip esp_adc esp_timer esp_driver_gpio
)
