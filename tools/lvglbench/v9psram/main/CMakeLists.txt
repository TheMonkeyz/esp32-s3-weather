idf_component_register(SRCS "../../bench.c" "../../../../main/lvgl_mem.c"
    EMBED_FILES "../../../../main/montserrat.ttf" "../../../../main/syllabics.ttf"
    WHOLE_ARCHIVE)   # LVGL calls lv_malloc_core() etc. from its own library: keep them
