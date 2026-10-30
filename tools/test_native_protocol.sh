#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Export ESP-IDF before running this protocol test}"
test_dir=$(mktemp -d /tmp/storybox-protocol-test.XXXXXX)
json_dir="$IDF_PATH/components/json/cJSON"
cc -std=c99 -I"$json_dir" -c "$json_dir/cJSON.c" -o "$test_dir/cJSON.o"
g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic -I"$json_dir" -Icomponents/hostlink/include -Icomponents/storage/include \
    test/native_protocol_test.cpp components/hostlink/protocol.cpp "$test_dir/cJSON.o" -o "$test_dir/protocol"
"$test_dir/protocol"
echo "Native USB system/file transfer protocol tests passed"
