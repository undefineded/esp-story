idf_component_register(SRCS "avi_index.cpp" "video_library.cpp" "player.cpp"
    INCLUDE_DIRS "include" REQUIRES storage audio esp_player esp_audio_render
    esp_video_render gmf_audio gmf_video esp_audio_codec esp_video_codec esp_extractor media_lib_sal)
