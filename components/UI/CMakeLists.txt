set(UI_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/HelloWorld)

file(GLOB_RECURSE SOURCES ${UI_SOURCE_DIR}/custom/*.c ${UI_SOURCE_DIR}/generated/*.c ${UI_SOURCE_DIR}/generated/images/*.c)

set(include_dirs 
    ${UI_SOURCE_DIR}/custom
    ${UI_SOURCE_DIR}/generated
    ${UI_SOURCE_DIR}/generated/guider_customer_fonts
    ${UI_SOURCE_DIR}/generated/guider_fonts   
    ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/lvgl__lvgl
    ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/lvgl__lvgl/src/font  
    )
idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${include_dirs}
                    REQUIRES lvgl
                    )
