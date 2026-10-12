"""Synthetic files only: no build, device, source mutation or credentials."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('release_web_archive',ROOT/'scripts/release-web-archive.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class ReleaseWebArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='release-web-archive-')
        self.addCleanup(self.temp.cleanup); self.root=Path(self.temp.name)
    def web(self,path,environment):
        path.mkdir(parents=True);(path/'index.html.gz').write_bytes(b'synthetic-entry')
        (path/'web-resource-manifest.json').write_text(json.dumps({'environment':environment}),encoding='utf-8')
    def candidate(self):
        candidate={'status':'PASS','inputsStable':True,'sourceDigest':'a'*64,'inputCount':2,'stages':[],'builds':[]}
        for name in ('native','host-unit','host-static','host-contracts',*module.ENVIRONMENTS):
            candidate['stages'].append({'name':name,'status':'PASS','exitCode':0})
        for env in module.ENVIRONMENTS:
            base=self.root/env;self.web(base/'web',env);images={}
            for name in ('firmware.bin','bootloader.bin','partitions.bin','littlefs.bin'):
                data=(env+name).encode();p=base/name;p.write_bytes(data)
                images[name]={'path':str(p),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            candidate['builds'].append(dict(environment=env,exitCode=0,sourceStable=True,
                sourceDigest=candidate['sourceDigest'],images=images,web=module.inspect_web(base/'web',env)))
        source=self.root/'source';source.mkdir()
        with zipfile.ZipFile(source/'source.zip','w') as archive:
            archive.writestr('src/synthetic.cpp','synthetic source')
            archive.writestr('test/synthetic.cpp','synthetic test')
        data=(source/'source.zip').read_bytes()
        manifest=dict(verified=True,sourceDigest=candidate['sourceDigest'],
                      archiveSha256=hashlib.sha256(data).hexdigest(),archiveBytes=len(data))
        (source/'manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
        integrity={'status':'INTEGRITY_PASS','candidateStatus':'PASS',**manifest,
                   'inputsVerified':2,'imagesVerified':36,'webTreesVerified':9}
        return candidate,integrity
    def test_copy_hashes_and_rejects_existing_archive(self):
        source=self.root/'source';self.web(source,'esp32-F4R0')
        result=module.archive_web(source,self.root/'archive','esp32-F4R0')
        self.assertEqual(result['webSha256'],module.inspect_web(source,'esp32-F4R0')['webSha256'])
        self.assertEqual(result['webFileCount'],2)
        with self.assertRaisesRegex(ValueError,'replace'):module.archive_web(source,self.root/'archive','esp32-F4R0')
    def test_environment_and_entry_are_mandatory(self):
        source=self.root/'source';self.web(source,'esp32-F4R0')
        with self.assertRaisesRegex(ValueError,'environment'):module.archive_web(source,self.root/'bad','esp32c3-F4R0')
        self.assertFalse((self.root/'bad').exists())
        (source/'index.html.gz').unlink()
        with self.assertRaisesRegex(ValueError,'entry'):module.inspect_web(source,'esp32-F4R0')
    def test_all_gates_and_integrity_required_before_deploy_adapter(self):
        candidate,integrity=self.candidate()
        rows=module.deployment_rows(candidate,integrity,self.root)
        self.assertEqual(len(rows),9)
        self.assertTrue(all(r['variant']=='canonical' and r['targetEnvironment']==r['environment'] for r in rows))
        for field,value in (('status','RUNNING'),('status','FAIL'),('inputsStable',False)):
            bad=copy.deepcopy(candidate);bad[field]=value
            with self.assertRaises(ValueError):module.deployment_rows(bad,integrity,self.root)
        for field,value in (('status','INTEGRITY_FAIL'),('candidateStatus','FAIL'),('sourceDigest','b'*64)):
            bad=copy.deepcopy(integrity);bad[field]=value
            with self.assertRaises(ValueError):module.deployment_rows(candidate,bad,self.root)
    def test_missing_or_failed_gate_and_duplicate_build_rejected(self):
        candidate,integrity=self.candidate()
        for mode in ('missing','failed','duplicate'):
            bad=copy.deepcopy(candidate)
            if mode=='missing':bad['stages'].pop(0)
            if mode=='failed':bad['stages'][0]['status']='FAIL'
            if mode=='duplicate':bad['builds'][-1]=bad['builds'][0]
            with self.assertRaises(ValueError):module.deployment_rows(bad,integrity,self.root)
    def test_changed_web_and_image_are_not_accepted(self):
        candidate,integrity=self.candidate();row=candidate['builds'][0]
        page=Path(row['web']['webDirectory'])/'index.html.gz';page.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'Web archive changed'):module.deployment_rows(candidate,integrity,self.root)
        row['web']=module.inspect_web(page.parent,row['environment'])
        Path(row['images']['firmware.bin']['path']).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'Image hash'):module.deployment_rows(candidate,integrity,self.root)
    def test_private_boundary_and_source_identity_required(self):
        candidate,integrity=self.candidate();row=candidate['builds'][0]
        with self.assertRaisesRegex(ValueError,'source archive'):module.deployment_rows(candidate,integrity,self.root/'other')
        row['sourceDigest']='b'*64
        with self.assertRaisesRegex(ValueError,'source mismatch'):module.deployment_rows(candidate,integrity,self.root)

    def test_missing_source_after_integrity_is_rejected(self):
        candidate,integrity=self.candidate()
        (self.root/'source/source.zip').unlink()
        with self.assertRaisesRegex(ValueError,'source archive'):
            module.deployment_rows(candidate,integrity,self.root)

    def test_replaced_source_after_integrity_is_rejected(self):
        candidate,integrity=self.candidate()
        with (self.root/'source/source.zip').open('ab') as stream:stream.write(b'changed')
        with self.assertRaisesRegex(ValueError,'Source archive no longer'):
            module.deployment_rows(candidate,integrity,self.root)

    def test_partial_integrity_counts_are_rejected(self):
        candidate,integrity=self.candidate()
        for key,value in (('inputsVerified',1),('imagesVerified',32),('webTreesVerified',8)):
            bad=copy.deepcopy(integrity);bad[key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'coverage'):
                module.deployment_rows(candidate,bad,self.root)

    def test_missing_and_invalid_source_identity_are_rejected(self):
        candidate,integrity=self.candidate()
        for value in (None,'','not-a-digest',0):
            bad=copy.deepcopy(candidate);bad['sourceDigest']=value
            witness=copy.deepcopy(integrity);witness['sourceDigest']=value
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'digest'):
                module.deployment_rows(bad,witness,self.root)

    def test_gate_exit_codes_duplicates_and_conflicting_status_are_rejected(self):
        candidate,integrity=self.candidate()
        for mode in ('no-exit','false-exit','failed-exit','duplicate','failed-stages','changed-inputs'):
            bad=copy.deepcopy(candidate)
            if mode=='no-exit':bad['stages'][0].pop('exitCode')
            elif mode=='false-exit':bad['stages'][0]['exitCode']=False
            elif mode=='failed-exit':bad['stages'][0]['exitCode']=1
            elif mode=='duplicate':bad['stages'].append(bad['stages'][0])
            elif mode=='failed-stages':bad['failedStages']=['native']
            else:bad['changedInputs']=['src/synthetic.cpp']
            with self.subTest(mode=mode),self.assertRaises(ValueError):
                module.deployment_rows(bad,integrity,self.root)

    def test_foreign_source_manifest_is_rejected(self):
        candidate,integrity=self.candidate()
        path=self.root/'source/manifest.json'
        manifest=json.loads(path.read_text(encoding='utf-8'));manifest['sourceDigest']='b'*64
        path.write_text(json.dumps(manifest),encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'Source archive no longer'):
            module.deployment_rows(candidate,integrity,self.root)

    def test_source_missing_entries_is_rejected_even_with_matching_hashes(self):
        candidate,integrity=self.candidate()
        source=self.root/'source/source.zip'
        with zipfile.ZipFile(source,'w') as archive:archive.writestr('src/synthetic.cpp','only one file')
        data=source.read_bytes();manifest=json.loads((source.parent/'manifest.json').read_text(encoding='utf-8'))
        for row in (manifest,integrity):
            row['archiveSha256']=hashlib.sha256(data).hexdigest();row['archiveBytes']=len(data)
        (source.parent/'manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'input set'):
            module.deployment_rows(candidate,integrity,self.root)

if __name__=='__main__':unittest.main()
