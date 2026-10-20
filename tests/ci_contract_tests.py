"""Offline CI-helper tests; does not clone dependencies, run native builds or modify repositories."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import tarfile
import os
import unittest
import xml.etree.ElementTree as ET
import yaml
from unittest.mock import patch

SOURCE=Path(__file__).resolve().parents[1]
def load(name):
    spec=importlib.util.spec_from_file_location(name,SOURCE/'tools'/f'{name}.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
prepare=load('prepare');runner=load('run')

class CIContracts(unittest.TestCase):
    def test_pinned_sdk_and_vcpkg(self):
        pins=json.loads((SOURCE/'tools/dependencies.json').read_text())
        self.assertEqual(len(pins['sdk']['sha256']),64)
        self.assertEqual(len(pins['vcpkg_commit']),40)
        self.assertTrue(pins['sdk']['url'].startswith('https://www.foobar2000.org/downloads/SDK-'))

    def test_verified_download_rejects_corrupt_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);original=root/'original';destination=root/'cached';original.write_bytes(b'trusted fixture')
            digest=hashlib.sha256(original.read_bytes()).hexdigest()
            prepare.verified_download(original.as_uri(),destination,digest)
            self.assertEqual(destination.read_bytes(),original.read_bytes())
            original.write_bytes(b'corrupt');destination.unlink()
            with self.assertRaises(RuntimeError):prepare.verified_download(original.as_uri(),destination,digest)
            self.assertFalse(destination.exists())

    def test_native_7zip_discovery_and_windows_installation_path(self):
        with patch.object(prepare.shutil,'which',side_effect=lambda name:'/tools/7zz' if name=='7zz' else None):self.assertEqual(prepare.sevenzip(),'/tools/7zz')
        with patch.object(prepare.shutil,'which',side_effect=lambda name:'/tools/7z' if name=='7z' else None):self.assertEqual(prepare.sevenzip(),'/tools/7z')
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'7-Zip/7z.exe';exe.parent.mkdir();exe.write_bytes(b'fixture')
            with patch.object(prepare.shutil,'which',return_value=None),patch.dict(os.environ,{'ProgramFiles':tmp},clear=True):self.assertEqual(prepare.sevenzip(),str(exe))
        with patch.object(prepare.shutil,'which',return_value=None),patch.dict(os.environ,{},clear=True),self.assertRaisesRegex(RuntimeError,'Native 7-Zip'):prepare.sevenzip()

    def test_sdk_native_extraction_handles_spaces_and_checks_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);archive=root/'SDK download.7z';archive.write_bytes(b'fixture');sdk=root/'SDK extracted'
            def extract(args):
                self.assertEqual(args,['/tools/7zz','x',archive,'-o'+str(sdk),'-y'])
                header=sdk/'foobar2000/SDK/foobar2000.h';header.parent.mkdir(parents=True);header.write_text('fixture')
            sdk.mkdir();(sdk/'stale').write_text('old dependency')
            with patch.object(prepare,'sevenzip',return_value='/tools/7zz'),patch.object(prepare,'run',side_effect=extract):prepare.extract_sdk(archive,sdk)
            self.assertFalse((sdk/'stale').exists())
            with patch.object(prepare,'sevenzip',return_value='/tools/7zz'),patch.object(prepare,'run'),self.assertRaisesRegex(RuntimeError,'SDK layout mismatch'):prepare.extract_sdk(archive,sdk)
            with patch.object(prepare,'sevenzip',return_value='/tools/7zz'),patch.object(prepare,'run',side_effect=RuntimeError('extract failed')),self.assertRaisesRegex(RuntimeError,'extract failed'):prepare.extract_sdk(archive,sdk)

    def test_shared_setup_provisions_native_sdk_extractor(self):
        action=yaml.load((SOURCE/'.github/actions/setup/action.yml').read_text(),Loader=yaml.BaseLoader)
        code=next(x['run'] for x in action['runs']['steps'] if x.get('name')=='Provision dependencies')
        for platform,native,available,install in [('darwin',True,False,True),('darwin',True,True,False),('darwin',False,False,False),('win32',True,False,False)]:
            with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'DMT_SOURCE':tmp,'DMT_NATIVE_BUILD':str(native).lower(),'DMT_ARCH':'x64'}),patch.object(prepare.sys,'platform',platform),patch.object(prepare.shutil,'which',return_value='/tools/7zz' if available else None),patch.object(prepare.subprocess,'run') as run:
                exec(compile(code,'setup action','exec'),{})
                calls=[x.args[0] for x in run.call_args_list]
                self.assertEqual(['brew','install','sevenzip'] in calls,install)
                self.assertEqual('--tests-only' in calls[-1],not native)
                self.assertNotIn('py7zr',str(calls))

    def test_sdk_include_patch_preserves_original_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);path=root/'dependency.vcxproj';ns='http://schemas.microsoft.com/developer/msbuild/2003'
            path.write_text(f'<Project xmlns="{ns}"><ItemDefinitionGroup><ClCompile><AdditionalIncludeDirectories>../..;..</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup></Project>')
            prepare.patch_sdk_includes(root,root/'include');prepare.patch_sdk_includes(root,root/'include')
            value=ET.parse(path).find('.//{'+ns+'}AdditionalIncludeDirectories').text
            self.assertEqual(value,str(root/'include')+';../..;..')

    def test_static_curl_layout_normalizes_debug_and_release(self):
        for release,debug in [('zs.lib','zsd.lib'),('zlib1.lib','zlibd1.lib'),('zlib.lib','zlibd.lib')]:
            with self.subTest(release=release),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);installed=root/'installed';dest=root/'curl'
                (installed/'include/curl').mkdir(parents=True)
                (installed/'include/curl/curl.h').write_text('header')
                for sub,curl,zlib in [('lib','libcurl.lib',release),('debug/lib','libcurl-d.lib',debug)]:
                    folder=installed/sub;folder.mkdir(parents=True)
                    (folder/curl).write_bytes(b'curl');(folder/zlib).write_bytes(sub.encode())
                prepare.normalized_curl(installed,dest,'x64')
                for config,sub in [('Release','lib'),('Debug','debug/lib')]:
                    self.assertEqual((dest/'lib/x64'/config/'libcurl.lib').read_bytes(),b'curl')
                    self.assertEqual((dest/'lib/x64'/config/'zlib.lib').read_bytes(),sub.encode())

    def test_apple_sanitizers_keep_checks_without_unsupported_leak_detection(self):
        workflow=yaml.load((SOURCE/'.github/workflows/sanitizers.yml').read_text(),Loader=yaml.BaseLoader)
        job=workflow['jobs']['sanitizers']
        self.assertTrue(job['runs-on'].startswith('macos-'))
        self.assertEqual(set(job['strategy']['matrix']['sanitizer']),{'address,undefined','thread'})
        step=next(x for x in job['steps'] if x.get('name')=='Run sanitizer suites')
        self.assertEqual(step['env']['ASAN_OPTIONS'],'detect_leaks=0:halt_on_error=1')
        self.assertEqual(step['env']['UBSAN_OPTIONS'],'halt_on_error=1:print_stacktrace=1')
        self.assertEqual(step['env']['TSAN_OPTIONS'],'halt_on_error=1')

    def test_sdk_factory_classes_allow_sdk_inheritance(self):
        import re
        for relative in ['main.cpp','platform/preferences_macos.mm','platform/preferences_windows.cpp']:
            text=(SOURCE/relative).read_text()
            classes=re.findall(r'FB2K_SERVICE_FACTORY\((\w+)\)|preferences_page_factory_t<(\w+)>',text)
            self.assertTrue(classes,relative)
            for pair in classes:
                name=next(x for x in pair if x)
                self.assertRegex(text,r'class\s+'+name+r'\s*:')

    def test_static_curl_layout_accepts_legacy_names_and_rejects_ambiguity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);installed=root/'installed';dest=root/'curl'
            (installed/'include/curl').mkdir(parents=True)
            for sub in ['lib','debug/lib']:
                folder=installed/sub;folder.mkdir(parents=True)
                (folder/'libcurl.lib').write_bytes(b'curl')
                (folder/'zlib.lib').write_bytes(b'zlib')
                (folder/'zlib-unrelated.lib').write_bytes(b'unrelated')
            prepare.normalized_curl(installed,dest,'Win32')
            (installed/'lib/zlib1.lib').write_bytes(b'ambiguous')
            with self.assertRaisesRegex(RuntimeError,'Expected one zlib.lib'):
                prepare.normalized_curl(installed,dest,'Win32')

    def test_workflow_matrix_is_complete_and_read_only(self):
        workflow=yaml.load((SOURCE/'.github/workflows/build-test.yml').read_text(),Loader=yaml.BaseLoader)
        self.assertEqual(workflow['permissions'],{'contents':'read'})
        self.assertEqual(set(workflow['on']),{'push','pull_request','workflow_dispatch'})
        entries=workflow['jobs']['native']['strategy']['matrix']['include']
        self.assertEqual(len(entries),4)
        self.assertEqual({(x['os'],x['arch'],x['configuration']) for x in entries},{(os,arch,config) for os,arch in [('windows-2022','x86'),('windows-2022','x64'),('macos-15-intel','x64'),('macos-15','arm64')] for config in ['Release']})
        steps=workflow['jobs']['native']['steps'];names=[x.get('name','') for x in steps]
        self.assertLess(names.index('Run offline suites and native IPC tests'),names.index('Build, inspect and stage native addon'))
        setup=[x for x in steps if x.get('uses')=='./.github/actions/setup']
        self.assertEqual(len(setup),1)
        self.assertEqual(setup[0]['with']['arch'],'${{ matrix.arch }}')
        action=yaml.load((SOURCE/'.github/actions/setup/action.yml').read_text(),Loader=yaml.BaseLoader)
        checkout=[x for x in action['runs']['steps'] if x.get('with',{}).get('repository')=='microsoft/vcpkg']
        self.assertEqual(checkout[0]['with']['ref'],'${{ steps.source.outputs.vcpkg_commit }}')
        self.assertIn("['vcpkg_commit']",action['runs']['steps'][0]['run'])

    def test_tests_only_provisioning_skips_sdk(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp);(source/'tools').mkdir()
            (source/'tools/dependencies.json').write_text(json.dumps({'vcpkg_commit':'fixture'}))
            with patch('sys.argv',['prepare','--source',str(source),'--vcpkg',str(source/'vcpkg'),'--arch','x64','--tests-only']),patch.object(prepare.subprocess,'check_output',return_value='fixture'),patch.object(prepare,'run') as run,patch.object(prepare,'verified_download') as download,patch.object(prepare,'patch_sdk_includes') as sdk,patch.object(prepare,'normalized_curl') as curl:
                prepare.main()
                download.assert_not_called();sdk.assert_not_called();curl.assert_not_called()
                self.assertTrue((source/'out/ci/state.json').is_file())
                self.assertNotIn('wtl',run.call_args.args[0])

    def test_workflow_python_steps_parse_and_never_publish(self):
        for file in list((SOURCE/'.github/workflows').glob('*.yml'))+[SOURCE/'.github/actions/setup/action.yml']:
            document=yaml.load(file.read_text(),Loader=yaml.BaseLoader)
            for job in document.get('jobs',{'composite':document.get('runs',{})}).values():
                for step in job['steps']:
                    if 'run' in step:compile(step['run'],str(file),'exec')
                    self.assertNotIn('gh release',step.get('run',''))
            self.assertNotIn('pull_request_target',document.get('on',{}))

    def test_native_artifacts_require_verified_successful_build(self):
        workflow=yaml.load((SOURCE/'.github/workflows/build-test.yml').read_text(),Loader=yaml.BaseLoader)
        steps=workflow['jobs']['native']['steps']
        build=next(x for x in steps if x.get('id')=='addon')
        upload=next(x for x in steps if x.get('uses','').startswith('actions/upload-artifact@'))
        self.assertLess(steps.index(build),steps.index(upload))
        self.assertNotIn('continue-on-error',build)
        self.assertNotIn('if',upload) # Default success() gating; never upload failed builds.
        self.assertEqual(upload['with']['path'],'${{ steps.addon.outputs.artifact_path }}')
        self.assertEqual(upload['with']['name'],'${{ steps.addon.outputs.artifact_name }}')
        self.assertEqual(upload['with']['if-no-files-found'],'error')
        self.assertEqual(upload['with']['retention-days'],'30')

    def test_windows_build_invokes_native_project_and_inspection(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp);output=source/'out/Win32/Release/discord-foobar-rpc.dll';output.parent.mkdir(parents=True);output.write_bytes(b'fixture')
            state={'arch':'x86','sdk':'sdk','json_include':'json'}
            with patch.object(runner.sys,'platform','win32'),patch.object(runner,'msbuild',return_value='MSBuild.exe'),patch.object(runner,'run') as command,patch.object(runner,'inspect_pe') as inspect:
                self.assertEqual(runner.build(source,state,'Release'),output)
                args=command.call_args.args[0];self.assertIn(source/'build/discord-foobar-rpc.vcxproj',args);self.assertIn('/t:Build',args);self.assertIn('/p:Platform=Win32',args)
                inspect.assert_called_once_with(output,'x86')
            output.unlink()
            with patch.object(runner.sys,'platform','win32'),patch.object(runner,'msbuild',return_value='MSBuild.exe'),patch.object(runner,'run'),self.assertRaises(RuntimeError):runner.build(source,state,'Release')

    def test_macos_build_checks_architecture_export_and_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp);output=source/'out/xcode/Build/Products/Release/discord-foobar-rpc.component';binary=output/'Contents/MacOS/discord-foobar-rpc';binary.parent.mkdir(parents=True);binary.write_bytes(b'fixture')
            state={'arch':'arm64','json_include':'json','sdk':'sdk'}
            with patch.object(runner.sys,'platform','darwin'),patch.object(runner,'run') as command,patch.object(runner.subprocess,'check_output',return_value='_foobar2000_get_interface'):
                self.assertEqual(runner.build(source,state,'Release'),output)
                calls=[x.args[0] for x in command.call_args_list]
                self.assertEqual(calls[0][0],'xcodebuild');self.assertIn('ARCHS=arm64',calls[0]);self.assertEqual(calls[-1],['codesign','--verify','--strict',output])
                self.assertIn(['lipo',binary,'-verify_arch','arm64'],calls)
            with patch.object(runner.sys,'platform','darwin'),patch.object(runner,'run'),patch.object(runner.subprocess,'check_output',return_value='wrong_symbol'),self.assertRaises(RuntimeError):runner.build(source,state,'Release')

    def test_artifacts_contain_real_output_and_checksums_for_every_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)
            for notice in ['LICENSE','THIRD_PARTY_NOTICES.md']:(source/notice).write_text('fixture notice')
            output=source/'native/discord-foobar-rpc.dll';output.parent.mkdir();output.write_bytes(b'native fixture')
            env=source/'github-output'
            for platform,arch in [('win32','x86'),('win32','x64'),('darwin','x64'),('darwin','arm64')]:
                for config in ['Debug','Release']:
                    native=output
                    if platform=='darwin':
                        native=source/'native/discord-foobar-rpc.component';binary=native/'Contents/MacOS/discord-foobar-rpc';binary.parent.mkdir(parents=True,exist_ok=True);binary.write_bytes(b'native fixture');binary.chmod(0o755)
                        (native/'Contents/Info.plist').write_text('fixture plist')
                    with patch.object(runner.sys,'platform',platform),patch.dict(os.environ,{'GITHUB_OUTPUT':str(env)}):folder=runner.stage_artifact(source,native,arch,config)
                    for line in (folder/'SHA256SUMS.txt').read_text().splitlines():
                        digest,name=line.split('  ',1);self.assertEqual(digest,hashlib.sha256((folder/name).read_bytes()).hexdigest())
                    if platform=='darwin':
                        with tarfile.open(folder/'discord-foobar-rpc.component.tar.gz') as archive:
                            entry=archive.getmember('discord-foobar-rpc.component/Contents/MacOS/discord-foobar-rpc');self.assertEqual(entry.mode&0o777,binary.stat().st_mode&0o777);self.assertEqual(archive.extractfile(entry).read(),b'native fixture')
                            self.assertIn('discord-foobar-rpc.component/Contents/Info.plist',archive.getnames())
                    else:self.assertEqual((folder/output.name).read_bytes(),b'native fixture')
            self.assertEqual(env.read_text().count('artifact_name='),8)
            output.write_bytes(b'')
            with patch.object(runner.sys,'platform','win32'),self.assertRaises(RuntimeError):runner.stage_artifact(source,output,'x64','Release')

    def test_native_projects_include_policy_and_shared_scheme(self):
        ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
        project=ET.parse(SOURCE/'build/discord-foobar-rpc.vcxproj')
        self.assertIn('..\\modules\\settings_policy.cpp',[x.attrib.get('Include') for x in project.findall('.//m:ClCompile',ns)])
        for kind in ['ClCompile', 'ClInclude', 'ResourceCompile']:
            for item in project.findall('.//m:' + kind, ns):
                if 'Include' in item.attrib:
                    self.assertTrue((SOURCE / 'build' / item.attrib['Include'].replace('\\', '/')).is_file())
        self.assertEqual(project.find('.//m:SourceRoot', ns).text, '$(MSBuildThisFileDirectory)..\\')
        pbx=(SOURCE/'build/discord-foobar-rpc.xcodeproj/project.pbxproj').read_text()
        self.assertIn('../modules/settings_policy.cpp',pbx)
        import re
        for path in re.findall(r'path = "(\.\./(?:modules/|platform/|main\.cpp)[^"]*)";', pbx):
            self.assertTrue((SOURCE / 'build' / path).is_file())
        self.assertIn('$(PROJECT_DIR)/../third_party/foobar-sdk', pbx)
        ET.parse(SOURCE/'build/discord-foobar-rpc.xcodeproj/xcshareddata/xcschemes/discord-foobar-rpc.xcscheme')
        workspace = ET.parse(SOURCE/'build/discord-foobar-rpc.xcworkspace/contents.xcworkspacedata')
        for item in workspace.findall('FileRef'):
            path = item.attrib['location'].removeprefix('group:')
            if path.endswith('discord-foobar-rpc.xcodeproj'):
                self.assertTrue((SOURCE / 'build' / path).is_dir())
            else:
                self.assertTrue(path.startswith('../third_party/foobar-sdk/'))

    def test_pe_architecture_and_export_checks(self):
        def image(arch,export=b'foobar2000_get_interface'):
            data=bytearray(2048);offset=128;struct.pack_into('<I',data,0x3c,offset);data[offset:offset+4]=b'PE\0\0'
            optional_size=240 if arch=='x64' else 224
            struct.pack_into('<HH',data,offset+4,0x8664 if arch=='x64' else 0x14c,1);struct.pack_into('<HH',data,offset+20,optional_size,0x2000)
            optional=offset+24;struct.pack_into('<H',data,optional,0x20b if arch=='x64' else 0x10b);directory=optional+(112 if arch=='x64' else 96);struct.pack_into('<I',data,directory,0x1000)
            section=optional+optional_size;struct.pack_into('<IIII',data,section+8,1024,0x1000,1024,512)
            struct.pack_into('<I',data,512+24,1);struct.pack_into('<I',data,512+32,0x1080);struct.pack_into('<I',data,640,0x10a0);data[672:672+len(export)]=export
            return data
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'fixture.dll'
            for arch in ['x86','x64']:
                path.write_bytes(image(arch));runner.inspect_pe(path,arch)
                with self.assertRaises(RuntimeError):runner.inspect_pe(path,'x64' if arch=='x86' else 'x86')
                path.write_bytes(image(arch,b'wrong_entrypoint'))
                with self.assertRaises(RuntimeError):runner.inspect_pe(path,arch)

if __name__=='__main__':unittest.main(verbosity=2)