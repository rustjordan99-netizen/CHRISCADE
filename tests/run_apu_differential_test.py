"""Execute baseline and optimized APU as Cortex-M0+ code, sample/state exact.

The copied reference is the pre-fine-tune local APU, NOT a rewritten model.
Requires project-local Unicorn via PYTHONPATH=.audio-test-tools.
"""
import os
from pathlib import Path
import re
import subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parents[1]
toolchain = (Path(os.environ['USERPROFILE']) / '.platformio' / 'packages' /
             'toolchain-rp2040-earlephilhower' / 'bin')
build = root / '.audio-test-build'
build.mkdir(exist_ok=True)
common = [str(toolchain / 'arm-none-eabi-g++.exe'), '-std=c++11', '-O3',
          '-Wall', '-Wextra', '-mcpu=cortex-m0plus', '-mthumb',
          '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
          '-ffunction-sections', '-fdata-sections',
          '-I' + str(root / 'tests'), '-I' + str(root / 'lib' / 'minigb_apu')]
objects = []
for label, flags in [('reference', ['-DAPU_REFERENCE']), ('actual', [])]:
    obj = build / f'apu_{label}.o'
    subprocess.run(common + flags + ['-c', str(root / 'tests' / 'apu_fixture.cpp'),
                                    '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / 'apu_differential_test.elf'
binary = build / 'apu_differential_test.bin'
subprocess.run(common + ['-nostdlib', '-Wl,-Ttext=0x1000',
    '-Wl,--entry=run_apu_case', str(root / 'tests' / 'apu_differential_test.cpp')] +
    objects + ['-lgcc', '-o', str(elf)], check=True)
subprocess.run([str(toolchain / 'arm-none-eabi-objcopy.exe'), '-O', 'binary',
                str(elf), str(binary)], check=True)
symbol_lines = subprocess.check_output(
    [str(toolchain / 'arm-none-eabi-nm.exe'), str(elf)], text=True)
symbols = {parts[2]: int(parts[0], 16) for line in symbol_lines.splitlines()
           if len(parts := line.split()) == 3}
emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
emu.mem_map(0x1000, 0x2F000)
emu.mem_write(0x1000, binary.read_bytes())
emu.mem_map(0x40000, 0x20000)

def call(name, arg=0):
    emu.reg_write(UC_ARM_REG_SP, 0x5FFF0)
    emu.reg_write(UC_ARM_REG_LR, 0x2F001)
    emu.reg_write(UC_ARM_REG_R0, arg)
    emu.emu_start(symbols[name] | 1, 0x2F000, count=100000000)
    assert emu.reg_read(UC_ARM_REG_PC) == 0x2F000, f'{name} did not return'
    return emu.reg_read(UC_ARM_REG_R0)

for case in range(512):
    result = call('run_apu_case', case)
    assert result == 0, f'APU mismatch case {case}, code {result}'
    if case % 128 == 127:
        print(f'PASS: {case + 1} cases, {(case + 1) * 8} frames compared', flush=True)
print('PASS: all stereo samples, register reads and internal state bytes match.', flush=True)

# Count guest ARM instructions using basic blocks and objdump widths. This is
# not a hardware cycle benchmark (no flash stalls, IRQ timing or bus contention).
disassembly = subprocess.check_output(
    [str(toolchain / 'arm-none-eabi-objdump.exe'), '-d', str(elf)], text=True)
widths = {}
for line in disassembly.splitlines():
    match = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{4})(?: ([0-9a-f]{4}))?\s', line)
    if match:
        widths[int(match[1], 16)] = 4 if match[3] else 2
counts = {}
for name in ('benchmark_reference', 'benchmark_actual'):
    # Hooks must exist before translation: a reused translated block may not
    # acquire a newly added block hook in Unicorn. Use a fresh engine here.
    emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    emu.mem_map(0x1000, 0x2F000)
    emu.mem_write(0x1000, binary.read_bytes())
    emu.mem_map(0x40000, 0x20000)
    count = [0]
    def block_hook(uc, address, size, user):
        end = address + size
        while address < end:
            count[0] += 1
            address += widths[address]
        assert address == end
    hook = emu.hook_add(UC_HOOK_BLOCK, block_hook)
    call('prepare_apu_benchmark')
    count[0] = 0
    call(name)
    emu.hook_del(hook)
    counts[name] = count[0]
    assert count[0] > 10000, 'Incomplete callback instrumentation'
    print(f'{name}: {count[0]:,} guest ARM instructions', flush=True)
gain = 100 * (1 - counts['benchmark_actual'] / counts['benchmark_reference'])
print(f'Synthetic four-channel callback: {gain:.1f}% fewer guest instructions (not MCU timing).')
