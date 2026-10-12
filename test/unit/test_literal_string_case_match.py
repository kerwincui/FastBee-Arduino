"""Compile the actual case bridge and compare its original String operation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
FIXTURES=ROOT/'test/unit/cpp'

class LiteralStringCaseMatchTests(unittest.TestCase):
    def test_actual_case_match_preserves_null_empty_case_utf8_and_long_inputs(self):
        oracle=json.loads((FIXTURES/'literal_string_case_match_before.json').read_text(encoding='utf-8'))
        harness=(FIXTURES/'literal_string_case_match_harness.cpp.in').read_text(encoding='utf-8')
        harness=harness.replace('@VALUES@',oracle['arrays']['values']).replace('@LITERALS@',oracle['arrays']['literals']).replace('@CASES@',str(oracle['comparisonCases'])).replace('@NULLCASES@',str(oracle['nullLiteralComparisons']))
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertTrue(compiler,'Real C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-literal-string-case-match-') as directory:
            temp=Path(directory);source=temp/'case-match.cpp';source.write_text(harness,encoding='utf-8')
            exe=temp/('case-match.exe' if os.name=='nt' else 'case-match')
            build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Os','-I'+str(ROOT/'test/mocks'),'-I'+str(ROOT/'include'),str(source),str(ROOT/'src/network/LiteralStringMatch.cpp'),'-o',str(exe)],capture_output=True,text=True,timeout=120)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            expected=f"PASS compared={oracle['comparisonCases']} null={oracle['nullLiteralComparisons']} actual case match method"
            self.assertIn(expected,run.stdout)

if __name__=='__main__':unittest.main()
