#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Export ESP-IDF before running this codec-adapter test}"
codec=managed_components/espressif__esp_audio_codec/include
test_dir=$(mktemp -d /tmp/storybox-mp3-test.XXXXXX)
g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic \
    -Icomponents/audio/include -Icomponents/storage/include \
    -I"$codec" -I"$codec/simple_dec" -I"$codec/decoder" -I"$codec/decoder/impl" \
    -I"$IDF_PATH/components/esp_common/include" -Ibuild/config \
    test/mp3_stream_test.cpp components/audio/mp3_stream.cpp -o "$test_dir/mp3_stream"
"$test_dir/mp3_stream"
echo "MP3 stream adapter fault tests passed (decoder algorithm not emulated)"
