# SPDX-License-Identifier: LGPL-2.1-or-later
import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest

file = Path(__file__).resolve().parents[2] / 'src/System/nexus-text-entry.py'
spec = importlib.util.spec_from_file_location('native_entry', file)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def request(fields=('', '', ''), limit=64, numeric=0, password=0, magic=module.MAGIC):
    encoded = [s.encode('utf-16-le') for s in fields]
    return module.HEADER.pack(magic, limit, numeric, password, *(len(x)//2 for x in encoded), 0, 0) + b''.join(encoded)


class NativeTextEntry(unittest.TestCase):
    def test_unicode_limit_counts_characters_not_bytes_or_utf16_units(self):
        model = module.read_request(io.BytesIO(request(('🏎 café', '東京', '🏎a'), limit=1)))
        self.assertEqual(model.title, '🏎 café')
        self.assertEqual(model.description, '東京')
        self.assertEqual(model.sanitize(model.initial), '🏎')

    def test_number_scope_filters_before_applying_limit(self):
        model = module.read_request(io.BytesIO(request(limit=9, numeric=1)))
        self.assertEqual(model.sanitize('12ab34.567890'), '123456789')
        self.assertEqual(model.sanitize(' １２٣x45'), '45')

    def test_generic_ten_character_field_is_not_guessed_to_be_numeric(self):
        model = module.read_request(io.BytesIO(request(limit=10)))
        self.assertEqual(model.sanitize('ab12'), 'ab12')

    def test_untrusted_labels_remain_data(self):
        text = '<img src="https://example.invalid/">'
        model = module.read_request(io.BytesIO(request((text, text, ''))))
        self.assertEqual(model.title, text)
        self.assertEqual(model.description, text)

    def test_malformed_and_truncated_requests_fail_before_ui(self):
        for data in [request(magic=0), request(limit=0), request(limit=module.MAX_FIELD_UNITS+1), request(numeric=3), request()[:-1], b'']:
            with self.subTest(data=data[:12]):
                with self.assertRaises((ValueError, EOFError)):
                    module.read_request(io.BytesIO(data))
        oversized = module.HEADER.pack(module.MAGIC, 64, 0, 0, module.MAX_FIELD_UNITS+1, 0, 0, 0, 0)
        with self.assertRaises(ValueError):
            module.read_request(io.BytesIO(oversized))

    def test_embedded_nul_is_not_silently_returned_as_shorter_text(self):
        with self.assertRaises(ValueError):
            module.read_request(io.BytesIO(request(('', '', 'a\0b'))))

    def test_results_include_utf8_terminator_but_cancel_returns_no_text(self):
        output = io.BytesIO()
        module.write_result(output, 0, '🏎 café')
        payload = '🏎 café'.encode() + b'\0'
        self.assertEqual(output.getvalue(), module.RESULT.pack(module.MAGIC, 0, len(payload)) + payload)
        output = io.BytesIO()
        module.write_result(output, module.CANCELLED, 'not returned')
        self.assertEqual(output.getvalue(), module.RESULT.pack(module.MAGIC, module.CANCELLED, 0))


if __name__ == '__main__':
    unittest.main()
