"""Execute real production user mutations with failing persistence; no hardware."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
from test_sensor_publication import definition
ROOT=Path(__file__).resolve().parents[2]
class UserPersistenceTests(unittest.TestCase):
 def test_failed_mutations_roll_back_and_success_commits(self):
  compiler=shutil.which('g++') or shutil.which('clang++')
  self.assertIsNotNone(compiler)
  source=(ROOT/'src/security/UserManager.cpp').read_text(encoding='utf-8')
  names=['addUser','deleteUser','updateUser','changePassword','resetPassword','setUserDescription','updateSecurityConfig']
  with tempfile.TemporaryDirectory(prefix='fastbee-user-save-') as directory:
   temp=Path(directory)
   (temp/'user_save.inc').write_text('\n'.join(definition(source,'bool UserManager::'+name+'(') for name in names),encoding='utf-8')
   exe=temp/('user-save.exe' if os.name=='nt' else 'user-save')
   built=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',f'-I{temp}',f'-I{ROOT/"test/mocks"}',str(ROOT/'test/unit/cpp/user_manager_persistence_harness.cpp'),'-o',str(exe)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
   self.assertEqual(built.returncode,0,built.stdout+built.stderr)
   result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)

