set(src_dirs
            WIFI
            PWM
)

set(include_dirs
            WIFI
            PWM
)

set(requires
            driver)

idf_component_register(SRC_DIRS ${src_dirs} 
                        INCLUDE_DIRS ${include_dirs} 
                        REQUIRES ${requires} 
                        PRIV_REQUIRES nvs_flash esp_wifi esp_timer)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)