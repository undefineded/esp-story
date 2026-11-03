idf_component_register(SRCS "src/app.cpp" "src/app_theme.cpp" "src/pages_home.cpp"
    "src/pages_settings.cpp" "src/pages_music.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display input storage media audio settings board esp_timer)
