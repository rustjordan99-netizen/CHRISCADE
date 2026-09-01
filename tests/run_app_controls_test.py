"""Execute production metronome and saved touch-control helpers on ARM."""
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
elf = build / 'app_controls_test.elf'
binary = build / 'app_controls_test.bin'
subprocess.run([
    str(toolchain / 'arm-none-eabi-g++.exe'), '-std=c++11', '-O2',
    '-Wall', '-Wextra', '-Werror', '-mcpu=cortex-m0plus', '-mthumb',
    '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
    '-ffunction-sections', '-fdata-sections', '-nostdlib',
    '-Wl,-Ttext=0x1000', '-Wl,--entry=run_app_controls_tests',
    '-Wl,--gc-sections', str(root / 'tests' / 'app_controls_test.cpp'),
    '-lgcc', '-o', str(elf)], check=True)
subprocess.run([str(toolchain / 'arm-none-eabi-objcopy.exe'),
                '-O', 'binary', str(elf), str(binary)], check=True)
symbols = subprocess.check_output(
    [str(toolchain / 'arm-none-eabi-nm.exe'), str(elf)], text=True)
entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
             if line.endswith(' run_app_controls_tests'))
emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
emu.mem_map(0x1000, 0x1F000)
emu.mem_write(0x1000, binary.read_bytes())
emu.mem_map(0x30000, 0x10000)
emu.reg_write(UC_ARM_REG_SP, 0x3FFF0)
emu.reg_write(UC_ARM_REG_LR, 0x1F001)
emu.emu_start(entry | 1, 0x1F000, count=30000000)
assert emu.reg_read(UC_ARM_REG_PC) == 0x1F000, 'Test did not return'
failure = emu.reg_read(UC_ARM_REG_R0)
assert failure == 0, f'App control assertion failed at C++ test line {failure}'
print('PASS: 3/4, 4/4, 6/8 sequences, wraparound, mode changes and first-beat accent.')
print('PASS: saved touch settings v1-v5 migration, independent toggles and invalid flags.')
