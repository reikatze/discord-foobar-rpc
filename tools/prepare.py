"""Provision pinned SDK/dependencies on CI runners. Never builds the addon or packages archives."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

def run(args,**kwargs):
    print('+', ' '.join(map(str,args)),flush=True)
    subprocess.run(list(map(str,args)),check=True,**kwargs)

def verified_download(url,destination,sha256):
    destination.parent.mkdir(parents=True,exist_ok=True)
    if not destination.exists() or hashlib.sha256(destination.read_bytes()).hexdigest()!=sha256:
        with urllib.request.urlopen(url,timeout=60) as response,destination.open('wb') as output:
            shutil.copyfileobj(response,output)
    if hashlib.sha256(destination.read_bytes()).hexdigest()!=sha256:
        destination.unlink(missing_ok=True)
        raise RuntimeError('Dependency checksum mismatch')

def patch_sdk_includes(sdk,include):
    # Only the freshly downloaded dependency workspace is changed, never the user's repository.
    namespace='http://schemas.microsoft.com/developer/msbuild/2003'
    ET.register_namespace('',namespace)
    q=lambda x:'{'+namespace+'}'+x
    for project in sdk.rglob('*.vcxproj'):
        tree=ET.parse(project)
        for group in tree.getroot().findall(q('ItemDefinitionGroup')):
            compiler=group.find(q('ClCompile'))
            if compiler is None:continue
            node=compiler.find(q('AdditionalIncludeDirectories'))
            if node is None:node=ET.SubElement(compiler,q('AdditionalIncludeDirectories'))
            old=node.text or '%(AdditionalIncludeDirectories)'
            if not old.startswith(str(include)+';'):node.text=str(include)+';'+old
        tree.write(project,encoding='utf-8',xml_declaration=True)

def normalized_curl(installed,destination,platform):
    shutil.copytree(installed/'include'/'curl',destination/'include'/'curl',dirs_exist_ok=True)
    for config,sub in [('Release','lib'),('Debug','debug/lib')]:
        folder=destination/'lib'/platform/config;folder.mkdir(parents=True,exist_ok=True)
        libs=list((installed/sub).glob('*.lib'))
        for prefix,target in [('libcurl','libcurl.lib'),('zlib','zlib.lib')]:
            selected=[p for p in libs if p.stem.lower().startswith(prefix)]
            if len(selected)!=1:raise RuntimeError(f'Expected one {prefix} static library, found {selected}')
            shutil.copy2(selected[0],folder/target)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--vcpkg',type=Path,required=True);parser.add_argument('--arch',choices=['x86','x64','arm64'],required=True);parser.add_argument('--tests-only',action='store_true');args=parser.parse_args()
    source=args.source.resolve();vcpkg=args.vcpkg.resolve();pins=json.loads((source/'tools/dependencies.json').read_text())
    actual=subprocess.check_output(['git','-C',str(vcpkg),'rev-parse','HEAD'],text=True).strip()
    if actual!=pins['vcpkg_commit']:raise RuntimeError('Unexpected vcpkg checkout revision')
    windows=sys.platform=='win32';triplet=args.arch+('-windows-static-md' if windows else '-osx')
    dependencies=source/'third_party';sdk=dependencies/'foobar-sdk';archive=dependencies/'downloads'/'foobar-sdk.7z'
    if not args.tests_only:
        verified_download(pins['sdk']['url'],archive,pins['sdk']['sha256'])
        if sdk.exists():shutil.rmtree(sdk)
        import py7zr
        with py7zr.SevenZipFile(archive,'r') as data:data.extractall(sdk)
        if not (sdk/'foobar2000/SDK/foobar2000.h').is_file():raise RuntimeError('SDK layout mismatch')
    if windows:run([vcpkg/'bootstrap-vcpkg.bat','-disableMetrics'],shell=True)
    else:run(['sh',vcpkg/'bootstrap-vcpkg.sh','-disableMetrics'])
    installed_root=dependencies/'vcpkg-installed'
    packages=['nlohmann-json']+(['curl[core,ssl]']+([] if args.tests_only else ['wtl']) if windows else [])
    command=[vcpkg/('vcpkg.exe' if windows else 'vcpkg'),'install',*packages,'--triplet='+triplet,'--x-install-root='+str(installed_root),'--disable-metrics']
    if windows:command+=['--overlay-triplets='+str(source/'tools/triplets')]
    run(command)
    installed=installed_root/triplet
    if windows and not args.tests_only:
        patch_sdk_includes(sdk,installed/'include')
        normalized_curl(installed,dependencies/'curl','Win32' if args.arch=='x86' else 'x64')
    state={'source':str(source),'sdk':str(sdk),'vcpkg':str(vcpkg),'installed':str(installed),'installed_root':str(installed_root),'triplet':triplet,'arch':args.arch,'json_include':str(installed/'include')}
    (source/'out/ci').mkdir(parents=True,exist_ok=True)
    (source/'out/ci/state.json').write_text(json.dumps(state,indent=2))

if __name__=='__main__':main()
