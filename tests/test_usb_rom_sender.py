"""Exercise the PC sender without opening a serial port or a real ROM file."""
import binascii
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('sender', Path(__file__).resolve().parents[1] / 'send_game_usb.py')
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


class FakePort:
    def __init__(self, reject=False, short=False):
        self.replies = []
        self.received = bytearray()
        self.header = None
        self.reject = reject
        self.short = short

    def reset_input_buffer(self):
        self.replies.clear()

    def write(self, data):
        if self.header is None:
            if data == b'HELLO\n':
                self.replies.extend([b'Initialize SD-Card ...\n', b'CCREADY\n'])
            else:
                tag, size, crc, name = data.decode().strip().split(' ')
                assert tag == 'CCROM1'
                self.header = int(size), int(crc, 16), bytes.fromhex(name).decode('utf-8')
                self.replies.append(b'ERR EXISTS_OR_SD\n' if self.reject else b'READY\n')
        else:
            assert len(data) <= 512
            if self.short:
                return len(data) - 1
            self.received.extend(data)
            self.replies.append(b'ACK\n')
            if len(self.received) == self.header[0]:
                assert binascii.crc32(self.received) == self.header[1]
                self.replies.append(b'DONE\n')
        return len(data)

    def readline(self):
        if not self.replies:
            raise AssertionError('Unexpected sender read')
        return self.replies.pop(0)


class SenderTests(unittest.TestCase):
    def test_exact_payload_and_unicode_filename(self):
        for length in (1, 511, 512, 513, 4097):
            payload = bytes(i % 256 for i in range(length))
            port = FakePort()
            with patch.object(Path, 'open', return_value=io.BytesIO(payload)):
                sender.send_rom(port, 'Pokémon Test.gbc', lambda message: None)
            self.assertEqual(port.received, payload)
            self.assertEqual(port.header[2], 'Pokémon Test.gbc')

    def test_existing_game_refused_before_payload(self):
        port = FakePort(reject=True)
        with patch.object(Path, 'open', return_value=io.BytesIO(b'example')):
            with self.assertRaisesRegex(RuntimeError, 'EXISTS_OR_SD'):
                sender.send_rom(port, 'Game.gb')
        self.assertFalse(port.received)

    def test_incomplete_write(self):
        with patch.object(Path, 'open', return_value=io.BytesIO(b'example')):
            with self.assertRaises(OSError):
                sender.send_rom(FakePort(short=True), 'Game.gb')

    def test_non_rom_rejected_without_opening_file(self):
        with patch.object(Path, 'open') as opening:
            with self.assertRaises(ValueError):
                sender.send_rom(FakePort(), 'game.sav')
            opening.assert_not_called()

    def test_empty_rom(self):
        with patch.object(Path, 'open', return_value=io.BytesIO()):
            with self.assertRaises(ValueError):
                sender.send_rom(FakePort(), 'Game.gb')


if __name__ == '__main__':
    unittest.main()
