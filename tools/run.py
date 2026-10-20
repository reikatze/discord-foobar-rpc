"""Run portable tests or build/inspect the native addon on Windows/macOS CI hosts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile
import xml.etree.ElementTree as ET

WINDOWS_DLL = 'foo_discord_foobar_rpc.dll'
MAC_EXECUTABLE = 'foo_discord_foobar_rpc'
MAC_BUNDLE = MAC_EXECUTABLE + '.component'

def run(args,**kwargs):
    print('+',' '.join(map(str,args)),flush=True)
    return subprocess.run(list(map(str,args)),check=True,**kwargs)

def msbuild():
    locator=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    value=subprocess.check_output([str(locator),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find','MSBuild/**/Bin/MSBuild.exe'],text=True).splitlines()
    if not value:raise RuntimeError('MSBuild not found')
    return value[0]

def tests(source,state,config,sanitizer):
    folder=source/'out/tests';command=['cmake','-S',source/'tests','-B',folder,'-DCMAKE_BUILD_TYPE='+config,'-DCMAKE_PREFIX_PATH='+state['installed']]
    if sys.platform=='win32':command+=['-A','Win32' if state['arch']=='x86' else 'x64','-DCMAKE_TOOLCHAIN_FILE='+str(Path(state['vcpkg'])/'scripts/buildsystems/vcpkg.cmake'),'-DVCPKG_TARGET_TRIPLET='+state['triplet'],'-DVCPKG_INSTALLED_DIR='+state['installed_root'],'-DVCPKG_MANIFEST_MODE=OFF']
    if sanitizer:command+=['-DMUSIC_SANITIZER='+sanitizer]
    run(command);run(['cmake','--build',folder,'--config',config,'--parallel','2'])
    report=source/'out/ci/test-results.xml'
    run(['ctest','--test-dir',folder,'--build-config',config,'--output-on-failure','--timeout','30','--output-junit',report])
    return report

def build(source,state,config):
    if sys.platform=='win32':
        platform='Win32' if state['arch']=='x86' else 'x64'
        run([msbuild(),source/'build/discord-foobar-rpc.vcxproj','/m:2','/t:Build','/p:Configuration='+config,'/p:Platform='+platform,'/p:FoobarSdkRoot='+state['sdk'],'/p:JsonIncludeDir='+state['json_include'],'/p:CurlRoot='+str(source/'third_party/curl'),'/p:VcpkgEnabled=false'])
        output=source/'out'/platform/config/WINDOWS_DLL
        if not output.is_file():raise RuntimeError('Native DLL was not produced')
        # Inspect PE headers/export table without loading it into an unsupported test host.
        inspect_pe(output,state['arch'])
    elif sys.platform=='darwin':
        derived=source/'out/xcode';arch='x86_64' if state['arch']=='x64' else 'arm64'
        run(['xcodebuild','-workspace',source/'build/discord-foobar-rpc.xcworkspace','-scheme','discord-foobar-rpc','-configuration',config,'-derivedDataPath',derived,'ARCHS='+arch,'ONLY_ACTIVE_ARCH=YES','CODE_SIGNING_ALLOWED=NO','JSON_INCLUDE_DIR='+state['json_include'],'FOOBAR_SDK_ROOT='+state['sdk'],'build'])
        output=derived/'Build/Products'/config/'foo_discord_foobar_rpc.component'
        executable=output/'Contents/MacOS/foo_discord_foobar_rpc'
        if not executable.is_file():raise RuntimeError('Native component bundle was not produced')
        run(['lipo',executable,'-verify_arch',arch])
        symbols=subprocess.check_output(['nm','-gU',str(executable)],text=True)
        if '_foobar2000_get_interface' not in symbols:raise RuntimeError('Component entrypoint export missing')
        run(['codesign','--force','--sign','-',output]);run(['codesign','--verify','--strict',output])
    else:raise RuntimeError('Native addon builds require Windows or macOS')
    return output

def stage_artifact(source,output,arch,config):
    platform={'win32':'windows','darwin':'macos'}.get(sys.platform)
    if not platform:raise RuntimeError('Native addon artifacts require Windows or macOS')
    supported={'windows':{'x86','x64'},'macos':{'x64','arm64'}}
    if arch not in supported[platform] or config not in {'Debug','Release'}:raise RuntimeError('Unsupported build target')
    if platform=='windows' and output.name!=WINDOWS_DLL:raise RuntimeError('Unexpected Windows component DLL filename')
    if platform=='macos' and output.name!=MAC_BUNDLE:raise RuntimeError('Unexpected macOS component bundle filename')
    binary=output/'Contents/MacOS/foo_discord_foobar_rpc' if platform=='macos' else output
    if not binary.is_file() or not binary.stat().st_size:raise RuntimeError('Native addon output missing or empty')
    name=f'discord-foobar-rpc-{platform}-{arch}-{config}'
    folder=source/'out/artifacts'/name
    if folder.exists():shutil.rmtree(folder)
    folder.mkdir(parents=True)
    if platform=='macos':
        # Artifact ZIP extraction loses executable modes. An inner tar preserves the signed bundle.
        payload=folder/'foo_discord_foobar_rpc.component.tar.gz'
        with tarfile.open(payload,'w:gz',dereference=False) as archive:archive.add(output,arcname=output.name)
    else:
        payload=folder/'discord-foobar-rpc.fb2k-component'
        entry=('x64/' if arch=='x64' else '')+output.name
        with zipfile.ZipFile(payload,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(output,arcname=entry)
    target=os.environ.get('GITHUB_OUTPUT')
    if target:
        with open(target,'a',encoding='utf-8') as f:f.write('artifact_name='+name+'\nartifact_path='+str(folder.resolve())+'\n')
    return folder

def inspect_pe(path,arch):
    import struct
    data=path.read_bytes();offset=struct.unpack_from('<I',data,0x3c)[0]
    if data[offset:offset+4]!=b'PE\0\0':raise RuntimeError('Not a PE binary')
    characteristics=struct.unpack_from('<H',data,offset+22)[0]
    if not characteristics & 0x2000:raise RuntimeError('PE image is not a DLL')
    machine,sections=struct.unpack_from('<HH',data,offset+4)
    if machine!=({'x86':0x14c,'x64':0x8664}[arch]):raise RuntimeError('Wrong DLL architecture')
    optional=offset+24;magic=struct.unpack_from('<H',data,optional)[0];directory=optional+(112 if magic==0x20b else 96)
    export_rva=struct.unpack_from('<I',data,directory)[0];optional_size=struct.unpack_from('<H',data,offset+20)[0];section_table=optional+optional_size
    def file_offset(rva):
        for i in range(sections):
            entry=section_table+i*40;virtual_size,virtual_address,raw_size,raw=struct.unpack_from('<IIII',data,entry+8)
            if virtual_address<=rva<virtual_address+max(virtual_size,raw_size):return raw+rva-virtual_address
        raise RuntimeError('Invalid PE RVA')
    exports=file_offset(export_rva);count=struct.unpack_from('<I',data,exports+24)[0];names=file_offset(struct.unpack_from('<I',data,exports+32)[0]);found=False
    for i in range(count):
        pos=file_offset(struct.unpack_from('<I',data,names+i*4)[0]);end=data.index(b'\0',pos);name=data[pos:end]
        if b'foobar2000_get_interface' in name:found=True
    if not found:raise RuntimeError('Component entrypoint export missing')

def summary(source,report=None,output=None,artifact=None):
    lines=['### discord-foobar-rpc CI']
    if report and report.exists():
        root=ET.parse(report).getroot();cases=root.findall('.//testcase');failures=sum(c.find('failure') is not None or c.find('error') is not None for c in cases)
        lines+=['',f'CTest suites: {len(cases)-failures}/{len(cases)} passed.']
        lines += ['- '+c.attrib.get('name','test')+': '+('failed' if c.find('failure') is not None or c.find('error') is not None else 'passed') for c in cases]
    if output:
        lines+=['',f'Native output verified: `{output.name}`.']
        binary=output/'Contents/MacOS/foo_discord_foobar_rpc' if output.is_dir() else output
        lines+=['SHA-256: `'+hashlib.sha256(binary.read_bytes()).hexdigest()+'`.']
    if artifact:lines+=['',f'Prepared artifact: `{artifact.name}`. Download it from this run’s Artifacts section after upload.']
    lines+=['','Native builds run on Windows/macOS runners. No release or repository write is produced.']
    target=os.environ.get('GITHUB_STEP_SUMMARY')
    if target:
        with open(target,'a',encoding='utf-8') as f:f.write('\n'.join(lines)+'\n')
    else:print('\n'.join(lines))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('operation',choices=['test','build','summary']);parser.add_argument('--source',type=Path,required=True);parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug');parser.add_argument('--sanitizer',default='');args=parser.parse_args();source=args.source.resolve()
    state=json.loads((source/'out/ci/state.json').read_text());report=source/'out/ci/test-results.xml'
    if args.operation=='test':
        try:tests(source,state,args.configuration,args.sanitizer)
        finally:summary(source,report=report)
    elif args.operation=='build':
        output=build(source,state,args.configuration)
        artifact=stage_artifact(source,output,state['arch'],args.configuration)
        summary(source,output=output,artifact=artifact)
    else:summary(source,report=report)
if __name__=='__main__':main()