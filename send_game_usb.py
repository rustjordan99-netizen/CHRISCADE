"""Send one new ROM to CHRISCADE's ADD GAME screen; never overwrite a file."""
import argparse
import binascii
from pathlib import Path
import time

CHUNK = 512
MAX_BYTES = 16 * 1024 * 1024


def expect(port, expected, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode('ascii', errors='replace').strip()
        if line == expected:
            return
        if line.startswith('ERR '):
            raise RuntimeError(f'Handheld: {line}. No existing game was overwritten.')
    raise TimeoutError(f'No {expected} reply. Open ADD GAME on the handheld and check USB.')


def send_rom(port, path, progress=print):
    path = Path(path)
    name = path.name.encode('utf-8')
    if path.suffix.lower() not in ('.gb', '.gbc', '.gbz') or len(name) > 255:
        raise ValueError('Choose a .gb, .gbc or .gbz ROM with a name under 256 UTF-8 bytes.')
    # Keep the same file open for checksum and transfer. The device verifies
    # the checksum before publishing it in the library.
    with path.open('rb') as rom:
        checksum = 0
        size = 0
        while True:
            block = rom.read(65536)
            if not block:
                break
            size += len(block)
            if size > MAX_BYTES:
                raise ValueError('Transfer limit is 16 MiB. This does not change the flash limit.')
            checksum = binascii.crc32(block, checksum)
        if not size:
            raise ValueError('The ROM is empty.')
        rom.seek(0)
        port.reset_input_buffer()
        port.write(b'HELLO\n')
        expect(port, 'CCREADY')
        header = f'CCROM1 {size} {checksum:08X} {name.hex()}\n'.encode('ascii')
        port.write(header)
        expect(port, 'READY')
        sent = 0
        last_percent = -1
        while sent < size:
            block = rom.read(min(CHUNK, size - sent))
            if not block:
                raise RuntimeError('ROM changed during transfer; cancel with B on the handheld.')
            if port.write(block) != len(block):
                raise IOError('Incomplete USB write; cancel with B on the handheld.')
            expect(port, 'ACK')
            sent += len(block)
            percent = sent * 100 // size
            if percent != last_percent and (percent % 5 == 0 or percent == 100):
                progress(f'{percent:3d}%  {path.name}')
                last_percent = percent
        expect(port, 'DONE')
    progress('Verified and added. Press B on CHRISCADE to return to the library.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', nargs='?', help='ROM file; opens a file picker when omitted')
    parser.add_argument('--port', help='CHRISCADE serial port, e.g. COM5')
    args = parser.parse_args()
    import serial
    from serial.tools import list_ports

    print('On CHRISCADE: Game Library > ADD GAME. Connect a USB DATA cable.')
    if not args.rom:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        args.rom = filedialog.askopenfilename(
            title='Choose one ROM to add to CHRISCADE',
            filetypes=[('Game Boy ROMs', '*.gb *.gbc *.gbz')])
        root.destroy()
        if not args.rom:
            return
    if not args.port:
        ports = list(list_ports.comports())
        if not ports:
            raise RuntimeError('No serial ports found. Check the USB data cable and handheld power.')
        for index, entry in enumerate(ports, 1):
            print(f'{index}: {entry.device} - {entry.description}')
        # Always ask: even one port might belong to an unrelated device.
        choice = int(input('Choose the CHRISCADE port number from this list: '))
        if choice < 1 or choice > len(ports):
            raise ValueError('Invalid port choice.')
        args.port = ports[choice - 1].device
    with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=10) as port:
        send_rom(port, args.rom)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f'\nTransfer stopped: {error}')
        raise SystemExit(1)
