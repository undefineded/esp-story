#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# POSIX helpers (fsync/lstat/symlink) so the suite also runs under MinGW on
# Windows; the header is a no-op elsewhere.
compat=(-include test/host_compat.h)
test_dir=$(mktemp -d /tmp/storybox-tests.XXXXXX)
for name in board_profile storage_access file_reader upload_transaction pcm_gain equalizer settings_record media_records parental_record story_timeline pn532_frame render_math transfer_crc transfer_frame; do
    g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic "${compat[@]}" -Icomponents/storage/include "test/${name}_test.cpp" -o "$test_dir/$name"
    "$test_dir/$name"
done
g++ -std=c++17 -Wall -Wextra "${compat[@]}" -Itest/catalog_host -Isrc \
    test/catalog_host/test_catalog.cpp src/catalog.cpp -o "$test_dir/catalog"
"$test_dir/catalog"
g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic "${compat[@]}" \
    -Icomponents/media/include -Icomponents/storage/include \
    test/media_library_test.cpp components/media/media_library.cpp -o "$test_dir/media"
"$test_dir/media"
g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic "${compat[@]}" \
    -Icomponents/media/include -Icomponents/storage/include -Isrc \
    test/media_cache_test.cpp components/media/media_library.cpp components/media/media_cache.cpp -o "$test_dir/media_cache"
"$test_dir/media_cache"
g++ -std=c++17 -pthread -Wall -Wextra -Werror -pedantic "${compat[@]}" \
    -Icomponents/video/include -Icomponents/storage/include \
    test/avi_index_test.cpp components/video/avi_index.cpp -o "$test_dir/avi"
"$test_dir/avi"
g++ -std=c++17 -Wall -Wextra -Werror -pedantic "${compat[@]}" \
    -Icomponents/settings/include test/parental_binding_test.cpp -o "$test_dir/parental_binding"
"$test_dir/parental_binding"
g++ -std=c++17 -Wall -Wextra -Werror -pedantic "${compat[@]}" \
    -Icomponents/power/include test/power_policy_test.cpp -o "$test_dir/power_policy"
"$test_dir/power_policy"
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -DARDUINO "${compat[@]}" \
    -Itest/pn532_host -Isrc test/pn532_host/test_pn532.cpp src/pn532.cpp -o "$test_dir/pn532"
"$test_dir/pn532"
echo "Host tests passed; executables: $test_dir"
