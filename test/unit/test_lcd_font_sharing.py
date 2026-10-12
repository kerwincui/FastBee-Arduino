"""Local font/linkage contracts; no claim of physical display acceptance."""
import ast
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def font_bytes(source, name):
    declaration = source.split('const uint8_t ' + name + '[', 1)[1]
    expected_size = int(declaration.split(']', 1)[0])
    lines = declaration.split('=', 1)[1].splitlines()
    result = bytearray()
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if not line.startswith(chr(34)):
            raise ValueError('Unexpected font definition')
        last = line.endswith(';')
        result.extend(ast.literal_eval('b' + (line[:-1] if last else line)))
        if last:
            break
    result.append(0)  # The C literal terminator is included in the declared size.
    if len(result) != expected_size:
        raise ValueError('Font byte length differs from the installed library')
    return bytes(result)


def glyphs(font):
    # U8g2 2.36.18 u8g2_font.c: 23-byte header, 8-bit glyph records,
    # Unicode lookup-table first offset, then 16-bit glyph records.
    result = {}
    position = 23
    while font[position + 1]:
        length = font[position + 1]
        if length < 2: raise ValueError('Invalid ASCII glyph length')
        result[font[position]] = font[position + 2:position + length]
        position += length
    position = 23 + int.from_bytes(font[21:23], 'big')
    position += int.from_bytes(font[position:position + 2], 'big')
    while int.from_bytes(font[position:position + 2], 'big'):
        encoding = int.from_bytes(font[position:position + 2], 'big')
        length = font[position + 2]
        if length < 3: raise ValueError('Invalid Unicode glyph length')
        result[encoding] = font[position + 3:position + length]
        position += length
    return result


class LcdFontSharingTests(unittest.TestCase):
    def test_compiled_policy_keeps_small_oled_builds_and_shares_screen_font(self):
        compiler = shutil.which('g++') or ('D:/msys64/ucrt64/bin/g++.exe' if Path('D:/msys64/ucrt64/bin/g++.exe').exists() else None)
        if not compiler: self.skipTest('Host C++ compiler unavailable')
        source = (ROOT/'src/peripherals/LCDManager.cpp').read_text(encoding='utf8')
        policy = source.split('const uint8_t* lcdChineseFont() {', 1)[1].split('}', 1)[0]
        policy = 'const uint8_t* lcdChineseFont() {' + policy + '}'
        prefix = '#include <cstdint>\nconst uint8_t u8g2_font_wqy12_t_gb2312[]={1};\nconst uint8_t u8g2_font_wqy12_t_gb2312a[]={2};\n'
        with tempfile.TemporaryDirectory() as temp:
            cpp=Path(temp)/'font.cpp';exe=Path(temp)/'font.exe'
            for screen, esp32 in ((0,False),(0,True),(1,False),(1,True)):
                expected = 1 if screen and esp32 else 2
                cpp.write_text(prefix + policy + '\nint main(){return *lcdChineseFont()==' + str(expected) + '?0:1;}\n')
                args=[compiler,'-std=c++11','-DFASTBEE_ENABLE_SCREEN_UI='+str(screen)]
                if esp32:args.append('-DARDUINO_ARCH_ESP32')
                subprocess.run(args+[str(cpp),'-o',str(exe)],check=True,capture_output=True)
                subprocess.run([str(exe)],check=True,capture_output=True)
        self.assertEqual(source.count('_display->setFont(lcdChineseFont());'),2)
        self.assertGreaterEqual(source.count('shouldEnable("lcdGb2312")'),2)
        self.assertIn('#if FASTBEE_ENABLE_LCD_GB2312\nconst uint8_t* lcdChineseFont()',source)

    def test_installed_full_font_preserves_every_subset_glyph_and_decoder_metrics(self):
        candidates=sorted((ROOT/'.pio/libdeps').glob('*/U8g2/src/clib/u8g2_fonts.c'))
        if not candidates:self.skipTest('Pinned U8g2 font source unavailable; build dependency required')
        # Some vendor copyright comments contain non-UTF8 bytes. Font literals
        # themselves use ASCII C escapes; decode losslessly, never ignore bytes.
        source=candidates[0].read_text(encoding='latin1')
        full=font_bytes(source,'u8g2_font_wqy12_t_gb2312')
        subset=font_bytes(source,'u8g2_font_wqy12_t_gb2312a')
        self.assertEqual(full[1:17],subset[1:17], 'Decode widths and font metrics must remain identical')
        full_glyphs, subset_glyphs=glyphs(full),glyphs(subset)
        self.assertGreater(len(subset_glyphs),3000)
        self.assertGreater(len(full_glyphs),len(subset_glyphs))
        for encoding, data in subset_glyphs.items():
            self.assertEqual(full_glyphs.get(encoding),data, 'Changed glyph U+%04X' % encoding)


if __name__=='__main__':unittest.main()
