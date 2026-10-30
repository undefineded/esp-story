idf_component_register(SRCS "pcm_output.cpp" "mp3_stream.cpp" "playback.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_audio_codec storage esp_timer)
