# Embed the generated .vlw fonts into the firmware.
#
# We do this ourselves instead of using board_build.embed_files because that helper
# passes an absolute path to objcopy, which makes the generated symbol name depend on
# the user's project location (_binary_c_Users_xxx_..._ui16_vlw_start). Running objcopy
# with cwd set to the fonts folder and a bare file name gives a stable symbol:
#   fonts/ui16.vlw  ->  _binary_ui16_vlw_start / _binary_ui16_vlw_end
#
# Regenerate the fonts with tools/gen_vlw_font.ps1

Import("env")

import os
import subprocess
from os.path import basename, join

FONTS = ["ui16", "ui15", "ui11", "ui14"]

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
font_dir = join(project_dir, "fonts")

mcu = env.BoardConfig().get("build.mcu", "esp32")
if mcu == "esp32s3":
    toolchain, prefix, arch, elf_fmt = "toolchain-xtensa-esp32s3", "xtensa-esp32s3-elf-objcopy", "xtensa", "elf32-xtensa-le"
elif mcu == "esp32s2":
    toolchain, prefix, arch, elf_fmt = "toolchain-xtensa-esp32s2", "xtensa-esp32s2-elf-objcopy", "xtensa", "elf32-xtensa-le"
else:
    toolchain, prefix, arch, elf_fmt = "toolchain-xtensa-esp32", "xtensa-esp32-elf-objcopy", "xtensa", "elf32-xtensa-le"

objcopy = join(env.PioPlatform().get_package_dir(toolchain), "bin", prefix)


def make_obj(target, source, env):
    """Run objcopy from the fonts directory so the symbol name stays stable."""
    subprocess.check_call(
        [
            objcopy,
            "--input-target", "binary",
            "--output-target", elf_fmt,
            "--binary-architecture", arch,
            "--rename-section", ".data=.rodata.embedded",
            basename(source[0].get_abspath()),
            target[0].get_abspath(),
        ],
        cwd=font_dir,
    )


for name in FONTS:
    src = join(font_dir, name + ".vlw")
    obj = join(build_dir, "font_" + name + ".o")
    if not os.path.isfile(src):
        print('Warning! missing font "%s" - run tools/gen_vlw_font.ps1' % src)
    node = env.Command(obj, src, env.VerboseAction(make_obj, "Embedding font %s" % name))
    env.AppendUnique(PIOBUILDFILES=[node])
    env.Depends("$PIOMAINPROG", node)