idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp32-camera esp_driver_uart
)

# main.c uses C++ features and C++ AI headers.
set_source_files_properties(main.c PROPERTIES LANGUAGE CXX)
