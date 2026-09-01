"""Run actual C++ UI/upload policy on Cortex-M0+, with fake SD entries only."""
import os
from pathlib import Path
import subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parents[1]
toolchain = Path(os.environ['USERPROFILE']) / '.platformio/packages/toolchain-rp2040-earlephilhower/bin'
build = root / '.audio-test-build'
build.mkdir(exist_ok=True)
elf, binary = build / 'library_update_test.elf', build / 'library_update_test.bin'
subprocess.run([
    str(toolchain / 'arm-none-eabi-g++.exe'), '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-mcpu=cortex-m0plus', '-mthumb', '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
    '-ffunction-sections', '-fdata-sections', '-nostdlib', '-Wl,-Ttext=0x1000',
    '-Wl,--entry=run_library_update_tests', '-Wl,--gc-sections',
    str(root / 'tests/library_update_test.cpp'), '-lgcc', '-o', str(elf)], check=True)
subprocess.run([str(toolchain / 'arm-none-eabi-objcopy.exe'), '-O', 'binary', str(elf), str(binary)], check=True)
symbols = subprocess.check_output([str(toolchain / 'arm-none-eabi-nm.exe'), str(elf)], text=True)
entry = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' run_library_update_tests'))
emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
emu.mem_map(0x1000, 0x1F000)
emu.mem_write(0x1000, binary.read_bytes())
emu.mem_map(0x30000, 0x10000)
emu.reg_write(UC_ARM_REG_SP, 0x3FFF0)
emu.reg_write(UC_ARM_REG_LR, 0x1F001)
emu.emu_start(entry | 1, 0x1F000, count=30000000)
assert emu.reg_read(UC_ARM_REG_PC) == 0x1F000, 'Test did not return'
failure = emu.reg_read(UC_ARM_REG_R0)
assert failure == 0, f'Library assertion failed at C++ test line {failure}'
print('PASS: cursor edits/UTF-8/capacity; both Pokeball masks; scrolling libraries of 0-30 games;')
print('      header/CRC checks; exclusive uploads; collision/cancel/write/sync/rename failures;')
print('      existing games, directories, temp files, saves and settings preserved in fake SD.')

# Inspect the actual ARM-generated pixels at both icon sizes, not a Python reimplementation.
preview_address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                       if line.endswith(' mushroom_preview'))
pixels = emu.mem_read(preview_address, 37 * 37 + 13 * 13)
from PIL import Image
preview = Image.new('RGB', (370, 240), (13, 16, 45))
palette = [(13, 16, 45), (30, 35, 49), (235, 55, 55), (255, 255, 255)]
offset = 0
for size, position, scale in [(18, (8, 8), 6), (6, (250, 72), 8)]:
    width = 2 * size + 1
    raster = pixels[offset:offset + width * width]
    offset += width * width
    # White connected components on the cap must be exactly three dots.
    white = {(x, y) for y in range(width) for x in range(width)
             if y - size <= 2 * size // 18 and raster[y * width + x] == 3}
    groups = []
    while white:
        todo = [white.pop()]
        area = 0
        while todo:
            x, y = todo.pop()
            area += 1
            for neighbor in [(x-1,y), (x+1,y), (x,y-1), (x,y+1)]:
                if neighbor in white:
                    white.remove(neighbor)
                    todo.append(neighbor)
        groups.append(area)
    assert len(groups) == 3, f'{size}: expected three cap spots, got {groups}'
    assert min(groups) >= (30 if size == 18 else 3), f'Spots too small: {groups}'
    icon = Image.new('RGB', (width, width))
    icon.putdata([palette[value] for value in raster])
    preview.paste(icon.resize((width * scale, width * scale), Image.Resampling.NEAREST), position)
preview.save(build / 'mushroom-preview.png')
print('PASS: wide mushroom symmetry and three large white cap spots at both sizes.')
print('Preview:', build / 'mushroom-preview.png')
