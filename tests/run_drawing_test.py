"""Run production canvas/export code on ARM, with simulated display and SD."""
import os
from pathlib import Path
import subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parents[1]
toolchain = Path(os.environ['USERPROFILE']) / '.platformio/packages/toolchain-rp2040-earlephilhower/bin'
build = root / '.audio-test-build'
build.mkdir(exist_ok=True)
elf, binary = build / 'drawing_test.elf', build / 'drawing_test.bin'
subprocess.run([
    str(toolchain / 'arm-none-eabi-g++.exe'), '-std=c++11', '-O2',
    '-Wall', '-Wextra', '-Werror', '-mcpu=cortex-m0plus', '-mthumb',
    '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
    '-ffunction-sections', '-fdata-sections', '-nostdlib',
    '-Wl,-Ttext=0x1000', '-Wl,--entry=run_drawing_tests', '-Wl,--gc-sections',
    str(root / 'tests/drawing_test.cpp'), '-lgcc', '-o', str(elf)], check=True)
subprocess.run([str(toolchain / 'arm-none-eabi-objcopy.exe'), '-O', 'binary', str(elf), str(binary)], check=True)
symbols = subprocess.check_output([str(toolchain / 'arm-none-eabi-nm.exe'), str(elf)], text=True)
def symbol(name):
    return next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' ' + name))
emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
emu.mem_map(0x1000, 0xFF000)
emu.mem_write(0x1000, binary.read_bytes())
emu.mem_map(0x200000, 0x10000)
emu.reg_write(UC_ARM_REG_SP, 0x20FFF0)
emu.reg_write(UC_ARM_REG_LR, 0xF0001)
emu.emu_start(symbol('run_drawing_tests') | 1, 0xF0000, count=500000000)
assert emu.reg_read(UC_ARM_REG_PC) == 0xF0000, 'Test did not return'
failure = emu.reg_read(UC_ARM_REG_R0)
assert failure == 0, f'Drawing assertion failed at C++ line {failure}'
print('PASS: thick strokes, all colors, clipping, memory guards, clear and exact displayed/saved pixels.')
print('PASS: header/payload, partial writes, sync/close/read/rename failures, collisions and drawing preservation.')
print('PASS: every GB/GBC screenshot pixel in all three scaling modes; source labels and shutter envelope.')
from PIL import Image
data = emu.mem_read(symbol('saved_picture') + 28, 320 * 240 * 2)
rgb = []
for offset in range(0, len(data), 2):
    color = data[offset] | data[offset + 1] << 8
    rgb.append((((color >> 11) & 31) * 255 // 31, ((color >> 5) & 63) * 255 // 63, (color & 31) * 255 // 31))
preview = Image.new('RGB', (320, 240))
preview.putdata(rgb)
preview.save(build / 'drawing-test-preview.png')
print('Preview:', build / 'drawing-test-preview.png')
