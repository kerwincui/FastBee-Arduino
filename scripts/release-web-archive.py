"""Archive one just-built Web tree; pure files only, never deploy a device.

Call under the repository firmware lock immediately after the matching buildfs.
The final deploy-report adapter requires a terminal, input-stable, fully-passed
candidate AND separately verified immutable archive integrity.
"""
import hashlib
import json
from pathlib import Path
import shutil
import re
import zipfile
from io import BytesIO

ENVIRONMENTS=frozenset(('esp32-F4R0','esp32-F8R0','esp32-F8R8','esp32c3-F4R0',
    'esp32c6-F4R0','esp32c6-F8R0','esp32s3-F8R0','esp32s3-F8R8','esp32s3-F16R8'))

def require(condition,message):
    if not condition: raise ValueError(message)

def inspect_web(path,environment):
    root=Path(path)
    require(root.is_dir() and not root.is_symlink(),'Web root must be a real directory')
    root=root.resolve()
    require((root/'index.html.gz').is_file(),'Missing compressed Web entry')
    manifest=json.loads((root/'web-resource-manifest.json').read_text(encoding='utf-8'))
    require(manifest.get('environment')==environment,'Web environment mismatch')
    rows=[]
    for item in sorted(root.rglob('*')):
        require(not item.is_symlink(),'Web tree must not contain symlinks')
        require(item.resolve().is_relative_to(root),'Web entry escapes root')
        if item.is_file():
            rows.append(item.relative_to(root).as_posix()+':'+hashlib.sha256(item.read_bytes()).hexdigest())
    digest=hashlib.sha256('\n'.join(sorted(rows)).encode('utf-8')).hexdigest()
    return dict(webDirectory=str(root),webSha256=digest,webFileCount=len(rows))

def archive_web(source,destination,environment):
    require(environment in ENVIRONMENTS,'Unknown canonical environment')
    before=inspect_web(source,environment)
    destination=Path(destination)
    require(not destination.exists(),'Refusing to replace an existing Web archive')
    shutil.copytree(source,destination)
    after=inspect_web(source,environment)
    copied=inspect_web(destination,environment)
    require(before==after,'Web source changed during archival')
    require(all(before[key]==copied[key] for key in ('webSha256','webFileCount')),'Web archive differs')
    return copied

def deployment_rows(candidate,integrity,private_root):
    require(candidate.get('status')=='PASS' and candidate.get('inputsStable') is True,
            'Candidate gates have not passed with stable inputs')
    require(isinstance(candidate.get('sourceDigest'),str) and
            re.fullmatch('[0-9a-f]{64}',candidate['sourceDigest']) is not None,
            'Candidate source digest is missing or invalid')
    require(integrity.get('status')=='INTEGRITY_PASS' and integrity.get('candidateStatus')=='PASS' and
            integrity.get('sourceDigest')==candidate.get('sourceDigest'),'Candidate integrity is not verified')
    stages=candidate.get('stages',[])
    require(stages and all(row.get('status')=='PASS' and type(row.get('exitCode')) is int and
                          row['exitCode']==0 for row in stages),'Failed or unfinished stage')
    require(len({row.get('name') for row in stages})==len(stages),'Duplicate candidate gate')
    require(not candidate.get('failedStages') and not candidate.get('changedInputs'),
            'Candidate still records failed stages or changed inputs')
    require({'native','host-unit','host-static','host-contracts',*ENVIRONMENTS}.issubset({row.get('name') for row in stages}),
            'Required candidate gate missing')
    builds=candidate.get('builds',[])
    require(len(builds)==len(ENVIRONMENTS) and {row.get('environment') for row in builds}==ENVIRONMENTS,
            'Nine unique canonical builds required')
    root=Path(private_root).resolve(); rows=[]
    # A prior integrity PASS is not enough if its private source archive was
    # removed, replaced or belongs to another candidate since verification.
    count=candidate.get('inputCount')
    require(type(count) is int and count>0 and integrity.get('inputsVerified')==count and
            integrity.get('imagesVerified')==len(ENVIRONMENTS)*4 and
            integrity.get('webTreesVerified')==len(ENVIRONMENTS),
            'Incomplete archive verification coverage')
    source=root/'source'/'source.zip'
    manifest_path=root/'source'/'manifest.json'
    for path in (source,manifest_path):
        require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root),
                'Missing or unsafe source archive')
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    source_bytes=source.read_bytes()
    source_sha=hashlib.sha256(source_bytes).hexdigest()
    require(manifest.get('verified') is True and manifest.get('sourceDigest')==candidate['sourceDigest'] and
            manifest.get('archiveSha256')==source_sha==integrity.get('archiveSha256') and
            manifest.get('archiveBytes')==len(source_bytes)==integrity.get('archiveBytes'),
            'Source archive no longer matches verified candidate')
    with zipfile.ZipFile(BytesIO(source_bytes)) as archive:
        names=archive.namelist()
        require(len(names)==count and len(set(names))==count,'Source archive input set is incomplete')
        require(all(name and not name.startswith('/') and ':' not in name and
                    chr(92) not in name and all(part not in ('','..','.') for part in name.split('/'))
                    for name in names),'Unsafe source archive entry')
        require(archive.testzip() is None,'Source archive CRC mismatch')
    for build in builds:
        environment=build['environment']
        require(type(build.get('exitCode')) is int and build['exitCode']==0 and build.get('sourceStable') is True and
                build.get('sourceDigest')==candidate['sourceDigest'],'Build source mismatch')
        images=build.get('images',{})
        require(set(images)=={'firmware.bin','bootloader.bin','partitions.bin','littlefs.bin'},'Incomplete flash images')
        for name,expected in images.items():
            path=Path(expected['path']).resolve()
            require(path.is_relative_to(root),'Image escapes private candidate archive')
            require(path==(root/environment/name).resolve(),'Image belongs to another environment')
            require(path.stat().st_size==expected['bytes'] and
                    hashlib.sha256(path.read_bytes()).hexdigest()==expected['sha256'],'Image hash mismatch: '+name)
        web=build.get('web',{})
        require(Path(web.get('webDirectory','')).resolve().is_relative_to(root),'Web escapes private archive')
        require(Path(web['webDirectory']).resolve()==(root/environment/'web').resolve(),'Web belongs to another environment')
        require(inspect_web(web['webDirectory'],environment)==web,'Web archive changed')
        rows.append(dict(environment=environment,targetEnvironment=environment,variant='canonical',
            exitCode=0,sourceStable=True,sourceDigest=candidate['sourceDigest'],
            firmwareSha256=images['firmware.bin']['sha256'],**web))
    return rows
