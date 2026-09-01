"""Compile the production queue for Cortex-M0+ and run it under Unicorn.

Run with PYTHONPATH pointing at the project-local .audio-test-tools dependency.
"""
import os
from pathlib import Path
import subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parents[1]
toolchain = (Path(os.environ['USERPROFILE']) / '.platformio' / 'packages' /
             'toolchain-rp2040-earlephilhower' / 'bin')
build = root / '.audio-test-build'
build.mkdir(exist_ok=True)
elf = build / 'audio_queue_test.elf'
binary = build / 'audio_queue_test.bin'
subprocess.run([
    str(toolchain / 'arm-none-eabi-g++.exe'), '-std=c++11', '-O2',
    '-Wall', '-Wextra', '-Werror', '-mcpu=cortex-m0plus', '-mthumb',
    '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
    '-ffunction-sections', '-fdata-sections', '-nostdlib',
    '-Wl,-Ttext=0x1000', '-Wl,--entry=run_audio_queue_tests',
    '-Wl,--gc-sections', str(root / 'tests' / 'audio_queue_test.cpp'),
    '-lgcc', '-o', str(elf)], check=True)
subprocess.run([str(toolchain / 'arm-none-eabi-objcopy.exe'),
                '-O', 'binary', str(elf), str(binary)], check=True)
symbols = subprocess.check_output(
    [str(toolchain / 'arm-none-eabi-nm.exe'), str(elf)], text=True)
entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
             if line.endswith(' run_audio_queue_tests'))
emulator = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
emulator.mem_map(0x1000, 0xF000)
emulator.mem_write(0x1000, binary.read_bytes())
emulator.mem_map(0x20000, 0x10000)
emulator.reg_write(UC_ARM_REG_SP, 0x2FFF0)
emulator.reg_write(UC_ARM_REG_LR, 0xF001)
emulator.emu_start(entry | 1, 0xF000, count=30000000)
assert emulator.reg_read(UC_ARM_REG_PC) == 0xF000, 'Test did not return'
failure = emulator.reg_read(UC_ARM_REG_R0)
assert failure == 0, f'Queue assertion failed at C++ test line {failure}'
print('PASS: production ARM queue; startup, delayed refill, underrun recovery,')
print('      mute/reset, and 50,000 randomized producer/consumer interleavings.')
