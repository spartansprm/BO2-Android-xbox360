#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
import hashlib
import importlib.util
import json
import os
import platform
import re
import secrets
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
MP_IMAGE_SHA256 = 'ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e'
IMAGE_BYTES = {'default_mp': 47054848, 'default': 38731776}
NDK_VERSION = '27.2.12479018'
BUILD_TOOLS = '35.0.0'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def execute(command, env=None, log_name=None):
    command = [str(value) for value in command]
    print('>', ' '.join(command), flush=True)
    if log_name:
        target = ROOT / 'work/build-logs' / (log_name + '.log')
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('w') as stream:
            status = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT).returncode
        if status:
            print('\n'.join(target.read_text(errors='replace').splitlines()[-30:]), file=sys.stderr)
            raise RuntimeError(f'{log_name} failed; see {target}')
    else:
        subprocess.run(command, cwd=ROOT, env=env, check=True)


def import_inputs(source, target, stems):
    expected = {stem + '.' + extension for stem in stems for extension in ('xex', 'xexp')}
    target.mkdir(parents=True, exist_ok=True)
    values = {}
    if source.is_dir():
        for name in expected:
            path = source / name
            if not path.is_file():
                raise ValueError(f'Missing {path}; supply extracted executables and their TU18 patches')
            if path.stat().st_size > 64 * 1024 * 1024:
                raise ValueError(f'Executable input exceeds 64 MiB: {name}')
            values[name] = path.read_bytes()
    else:
        with zipfile.ZipFile(source) as archive:
            selected = {}
            total = 0
            for info in archive.infolist():
                path = PurePosixPath(info.filename)
                if path.is_absolute() or '..' in path.parts or '\\' in info.filename:
                    raise ValueError('Unsafe input ZIP path')
                if info.is_dir():
                    continue
                if info.filename == 'manifest.json':
                    if info.file_size > 65536:
                        raise ValueError('Input manifest exceeds 64 KiB')
                    continue
                name = path.name
                allowed = {s + '.' + e for s in IMAGE_BYTES for e in ('xex', 'xexp')}
                if name not in allowed or len(path.parts) > 2 or (len(path.parts) == 2 and path.parts[0] != 'game'):
                    raise ValueError(f'Unexpected input ZIP entry: {info.filename}')
                if name in selected:
                    raise ValueError('Duplicate executable in input ZIP: ' + name)
                total += info.file_size
                if info.file_size > 64 * 1024 * 1024 or total > 128 * 1024 * 1024:
                    raise ValueError('Input ZIP exceeds its size limit')
                selected[name] = info
            if not expected.issubset(selected):
                raise ValueError('Input ZIP is missing: ' + ', '.join(sorted(expected - selected.keys())))
            values = {name: archive.read(selected[name]) for name in expected}
    for name, data in values.items():
        if not data.startswith(b'XEX2') or len(data) < 24:
            raise ValueError('Invalid XEX2 executable or patch: ' + name)
    for name, data in values.items():
        path = target / name
        if not path.is_file() or path.read_bytes() != data:
            temporary = path.with_suffix(path.suffix + '.part')
            temporary.write_bytes(data)
            temporary.replace(path)
    return {name: digest(target / name) for name in sorted(expected)}


def validate_image(stem, directory):
    image = directory / (stem + '.image.bin')
    summary = json.loads((directory / (stem + '.summary.json')).read_text())
    if image.stat().st_size != IMAGE_BYTES[stem] or summary.get('title_id') != '415608C3' or not summary.get('patch_applied'):
        raise ValueError(f'{stem}: the loaded image is not the supported TU18 program')
    sha = digest(image)
    if stem == 'default_mp' and sha != MP_IMAGE_SHA256:
        raise ValueError('The patched Multiplayer/Zombies image does not match the supported TU18 image')
    if stem == 'default_mp' and summary.get('version') != '0.0.18.14':
        raise ValueError(f'{stem}: Title Update 18 is required')
    (directory / (stem + '.image.sha256')).write_text(sha + '\n')
    return sha


def generation_fingerprint(stem, input_hashes):
    paths = []
    for directory in ['sdk/src/codegen', 'sdk/include/rex/codegen', 'sdk/resources/templates', 'tools/host_codegen']:
        paths.extend(p for p in (ROOT / directory).rglob('*') if p.is_file())
    project = 'app' if stem == 'default_mp' else 'campaign'
    paths.extend((ROOT / project).glob('*.toml'))
    for name in ['codegen.sh','read_before_write.py','setjmp_locals.py','direct_calls.py','localize.py','guest_barriers.py','codegen_gaps.py','check_android_guest_fences.py']:
        path = ROOT / 'tools' / name
        if path.is_file():paths.append(path)
    paths.append(ROOT / 'campaign/codegen.sh')
    for relative in ['sdk/src/rexglue/commands/project_scan.cpp','work/host-codegen/bo2-codegen']:
        path=ROOT/relative
        if path.is_file():paths.append(path)
    local = ROOT / 'app/locals.txt'
    if local.is_file():paths.append(local)
    value = hashlib.sha256(json.dumps(input_hashes, sort_keys=True).encode())
    for path in sorted(set(paths)):
        value.update(str(path.relative_to(ROOT)).encode() + b'\0' + bytes.fromhex(digest(path)))
    return value.hexdigest()


def build_tools(args):
    cmake = shutil.which(args.cmake)
    ninja = shutil.which(args.ninja)
    cxx = shutil.which(args.cxx)
    if not all((cmake, ninja, cxx, shutil.which('git'), shutil.which('java'), shutil.which('keytool'))):
        raise ValueError('Install Git, GCC 14, JDK 21 and requirements-build.txt; activate your Python environment')
    version = subprocess.check_output([cmake, '--version'], text=True).splitlines()[0]
    parsed = re.search(r'(\d+)\.(\d+)', version)
    if not parsed or tuple(map(int, parsed.groups())) < (3, 25):
        raise ValueError('CMake 3.25 or newer is required')
    if importlib.util.find_spec('capstone') is None:
        raise ValueError('Install the Python dependencies: python3 -m pip install -r requirements-build.txt')
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or '').expanduser()
    if not str(sdk) or not (sdk / 'platforms/android-35/android.jar').is_file():
        raise ValueError('Set ANDROID_HOME to an SDK containing platforms;android-35')
    sdk = sdk.resolve()
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', sdk / 'ndk' / NDK_VERSION)).expanduser().resolve()
    if not (ndk / 'build/cmake/android.toolchain.cmake').is_file():
        raise ValueError(f'Install ndk;{NDK_VERSION} or set ANDROID_NDK_HOME')
    tools = sdk / 'build-tools' / BUILD_TOOLS
    if not all((tools / name).is_file() for name in ('apksigner', 'zipalign', 'aapt2')):
        raise ValueError(f'Install build-tools;{BUILD_TOOLS}')
    if not (sdk/'cmake/3.22.1/bin/cmake').is_file():
        raise ValueError('Install cmake;3.22.1 in the Android SDK for the Gradle native build')
    env = dict(os.environ, CXX=cxx, PYTHON=sys.executable, ANDROID_HOME=str(sdk), ANDROID_NDK_HOME=str(ndk),
               BO2_RUNTIME_CMAKE=cmake, BO2_RUNTIME_NINJA=ninja, BO2_BUILD_JOBS=str(args.jobs))
    return cmake, ninja, env, tools


def generate(args, input_hashes, env, cmake, ninja, stems):
    host = ROOT / 'work/host-xex-verifier'
    loader = host / 'xexdump'
    if not loader.is_file():
        execute([sys.executable, ROOT / 'tools/setup_host_xex_verifier.py'], env, 'host-loader')
    execute([sys.executable, ROOT / 'tools/setup_host_codegen.py', '--host-sdk', host / 'linux-amd64',
             '--cmake', cmake, '--ninja', ninja, '--cxx', env['CXX'], '--jobs', args.jobs], env, 'host-codegen')
    env = dict(env, BO2_CODEGEN=str(ROOT / 'work/host-codegen/bo2-codegen'))
    dump = ROOT / 'work/dump_tu18'
    dump.mkdir(parents=True, exist_ok=True)
    for stem in stems:
        hashes = {name:value for name,value in input_hashes.items() if name.startswith(stem + '.')}
        stamp = ROOT / 'work' / (stem + '-generation.json')
        fingerprint = generation_fingerprint(stem, hashes)
        gen = ROOT / ('app/generated/default_mp' if stem == 'default_mp' else 'campaign/generated/default')
        prefix = 'bo2zm' if stem == 'default_mp' else 'bo2cp'
        required = [gen/'sources.cmake', gen/(prefix+'_init.h'), gen/(prefix+'_pch.h'), gen/(prefix+'_recomp.0.cpp')]
        if not args.force and stamp.is_file() and all(p.is_file() for p in required):
            saved = json.loads(stamp.read_text())
            if saved.get('fingerprint') == fingerprint:
                validate_image(stem, dump)
                print(stem + ': generated sources are current', flush=True)
                continue
        if gen.is_symlink() or gen.parent.is_symlink():
            raise ValueError('Generated directories must belong to this checkout: '+str(gen))
        execute([loader, ROOT/'work/load_tu18', stem+'.xex', dump], env, stem+'-loader')
        validate_image(stem, dump)
        script = ROOT / ('tools/codegen.sh' if stem == 'default_mp' else 'campaign/codegen.sh')
        gaps = ROOT / ('app/gaps.toml' if stem == 'default_mp' else 'campaign/gaps.toml')
        before = digest(gaps)
        execute(['bash', script], env, stem+'-codegen')
        if digest(gaps) != before:
            execute(['bash', script], env, stem+'-codegen-second-pass')
        if not all(p.is_file() for p in required):
            raise RuntimeError(stem + ': code generation did not produce all required sources')
        if stem == 'default_mp':
            execute([sys.executable, ROOT/'tools/check_android_guest_fences.py'], env, 'guest-fences')
        stamp.write_text(json.dumps({'fingerprint':generation_fingerprint(stem, hashes), 'inputs':hashes}, indent=2)+'\n')


def native(args, env, cmake, ninja, stems):
    if args.stage!='game':
        execute(['bash', ROOT/'tools/build_android_native_sdk.sh'], env, 'android-runtime')
    toolchain = ['-DCMAKE_MAKE_PROGRAM='+ninja,
                 '-DCMAKE_TOOLCHAIN_FILE='+env['ANDROID_NDK_HOME']+'/build/cmake/android.toolchain.cmake',
                 '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',
                 '-DCMAKE_BUILD_TYPE=Release', '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON']
    for stem in stems:
        cp = stem == 'default'
        recomp = ROOT / ('work/android-recomp-cp' if cp else 'work/android-recomp')
        game = ROOT / ('work/android-game-cp' if cp else 'work/android-game')
        if args.stage!='game':
            execute([cmake,'-S',ROOT/'android/recomp','-B',recomp,'-G','Ninja',*toolchain,
                     '-DBO2_RECOMP_PROJECT='+('bo2cp' if cp else 'bo2zm')],env,stem+'-recomp-configure')
            execute([cmake,'--build',recomp,'-j',args.jobs],env,stem+'-recomp-build')
        options = ['-DBO2_ANDROID_RUNTIME_DIR='+str(ROOT/'work/android-runtime'), '-DBO2_ANDROID_RECOMP_DIR='+str(recomp)]
        options += args.cmake_arg
        if cp:
            hash_file=ROOT/'work/dump_tu18/default.image.sha256'
            if not hash_file.is_file():raise ValueError('Generate the campaign first; its image fingerprint is missing')
            image_sha=hash_file.read_text().strip()
            if not re.fullmatch('[0-9a-f]{64}',image_sha):raise ValueError('Invalid campaign image fingerprint')
            if (ROOT/'work/dump_tu18/default.image.bin').is_file():
                if validate_image(stem,ROOT/'work/dump_tu18')!=image_sha:raise ValueError('Campaign image fingerprint mismatch')
            options += ['-DBO2_ANDROID_CAMPAIGN=ON','-DBO2CP_ANDROID_IMAGE_SHA256='+image_sha]
        execute([cmake,'-S',ROOT/'android/game','-B',game,'-G','Ninja',*toolchain,*options],env,stem+'-game-configure')
        execute([cmake,'--build',game,'--target','bo2cp' if cp else 'bo2game','-j',args.jobs],env,stem+'-game-build')
        library=game/('libbo2cp.so' if cp else 'libbo2game.so')
        execute([Path(env['ANDROID_NDK_HOME'])/'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip',
                 '--strip-unneeded',library],env,stem+'-game-strip')


def local_signing(env, args):
    if args.keystore:
        if not args.keystore.is_file():raise ValueError('Signing keystore is missing')
        password = os.environ.get(args.store_pass_env)
        if not password:raise ValueError('Set the signing password in '+args.store_pass_env)
        return args.keystore.resolve(), args.key_alias, dict(env, BO2_APK_STORE_PASS=password)
    folder = ROOT / 'work/signing'
    folder.mkdir(parents=True,exist_ok=True)
    keystore = folder / 'local-release.p12'
    password_file = folder / 'password'
    if keystore.exists() != password_file.exists():
        raise ValueError('Incomplete local signing configuration in work/signing; restore the matching key and password')
    if not password_file.exists():
        fd=os.open(password_file,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
        with os.fdopen(fd,'w') as stream:stream.write(secrets.token_urlsafe(32)+'\n')
    signing_env = dict(env,BO2_APK_STORE_PASS=password_file.read_text().strip())
    if not keystore.exists():
        execute(['keytool','-genkeypair','-noprompt','-storetype','PKCS12','-keystore',keystore,'-alias','bo2port',
                 '-keyalg','RSA','-keysize','3072','-validity','10000','-dname','CN=BO2 Android local build',
                 '-storepass:env','BO2_APK_STORE_PASS','-keypass:env','BO2_APK_STORE_PASS'],signing_env,'signing-key')
        keystore.chmod(0o600)
    return keystore,'bo2port',signing_env


def apk(args, env, tools, input_hashes, stems):
    execute([sys.executable,ROOT/'tools/setup_dxc.py','--out',ROOT/'out/dxc'],env,'shader-compiler')
    options = ['-Pbo2RuntimeDir='+str(ROOT/'work/android-runtime'),'-Pbo2GameDir='+str(ROOT/'work/android-game'),
               '-Pbo2MpXexSha256='+input_hashes['default_mp.xex'], '-Pbo2MpXexpSha256='+input_hashes['default_mp.xexp']]
    if 'default' in stems:options += ['-Pbo2CampaignDir='+str(ROOT/'work/android-game-cp')]
    if args.application_id:options += ['-Pbo2ApplicationId='+args.application_id]
    execute(['bash',ROOT/'android/gradlew','-p',ROOT/'android','--no-daemon',*options,
             ':app:assembleRelease',':app:lintRelease',':app:testReleaseUnitTest'],env,'android-apk')
    unsigned = ROOT/'android/app/build/outputs/apk/release/app-release-unsigned.apk'
    if not unsigned.is_file():raise RuntimeError('The release APK was not produced')
    required = {'lib/arm64-v8a/libbo2game.so'}
    if 'default' in stems:required.add('lib/arm64-v8a/libbo2cp.so')
    finish_apk(args,env,tools,unsigned,'BO2-v0.55.0.apk',required)


def finish_apk(args,env,tools,unsigned,name,required):
    key,alias,signing_env = local_signing(env,args)
    output = ROOT/'dist'/name
    output.parent.mkdir(exist_ok=True)
    execute([tools/'apksigner','sign','--ks',key,'--ks-key-alias',alias,'--ks-pass','env:BO2_APK_STORE_PASS',
             '--key-pass','env:BO2_APK_STORE_PASS','--out',output,unsigned],signing_env,'apk-sign')
    execute([tools/'apksigner','verify','--verbose','--print-certs',output],env,'apk-signature')
    execute([tools/'zipalign','-c','-P','16','4',output],env,'apk-alignment')
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:raise RuntimeError('APK ZIP integrity failed')
        if not required.issubset(archive.namelist()):raise RuntimeError('A game library is missing from the APK')
        if not required and any(n.endswith('.so') for n in archive.namelist()):
            raise RuntimeError('The input extractor must contain no native libraries')
    print('APK: '+str(output),flush=True)
    print('SHA-256: '+digest(output),flush=True)


def extractor(args,env,tools):
    execute([sys.executable,ROOT/'tools/setup_android_diagnostic.py'],env,'extractor-dependencies')
    execute(['bash',ROOT/'android/gradlew','-p',ROOT/'android','--no-daemon','-Pbo2InputExtractor=true',
             '-Pbo2ApplicationId=org.bo2port.inputextractor',':app:assembleRelease',':app:lintRelease'],env,'input-extractor')
    unsigned=ROOT/'android/app/build/outputs/apk/release/app-release-unsigned.apk'
    finish_apk(args,env,tools,unsigned,'BO2-input-extractor.apk',set())


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build Black Ops II Android from source and your own TU18 executables.')
    parser.add_argument('--inputs',type=Path,help='Directory or ZIP with default_mp.xex/.xexp and default.xex/.xexp')
    parser.add_argument('--campaign-inputs',type=Path,help='Separate campaign executable directory or exported ZIP')
    parser.add_argument('--program',choices=['all','mp','campaign'],default='all')
    parser.add_argument('--without-campaign',action='store_true')
    parser.add_argument('--stage',choices=['all','inputs','generate','native','game','apk','extractor','check'],default='all')
    parser.add_argument('--force',action='store_true',help='Regenerate sources even when the inputs are unchanged')
    parser.add_argument('--jobs',type=int,default=min(4,os.cpu_count() or 2))
    parser.add_argument('--cmake',default=os.environ.get('BO2_RUNTIME_CMAKE','cmake'))
    parser.add_argument('--ninja',default=os.environ.get('BO2_RUNTIME_NINJA','ninja'))
    parser.add_argument('--cxx',default=os.environ.get('CXX','g++-14'))
    parser.add_argument('--cmake-arg',action='append',default=[],help='Additional android/game CMake definition; use --cmake-arg=-DNAME=VALUE')
    parser.add_argument('--keystore',type=Path)
    parser.add_argument('--key-alias',default='bo2port')
    parser.add_argument('--store-pass-env',default='BO2_STORE_PASS')
    parser.add_argument('--application-id',help='Optional package identifier for a separately installable fork')
    args=parser.parse_args(argv)
    if args.jobs<1:parser.error('--jobs must be positive')
    if args.program=='campaign' and args.stage in ('all','apk'):
        parser.error('Use --program all to package the APK; campaign-only selection supports inputs, generate and native')
    if args.application_id and not re.fullmatch(r'[A-Za-z]\w*(?:\.[A-Za-z]\w*)+',args.application_id):parser.error('Invalid application identifier')
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','AMD64'):
        parser.error('Build on Linux x86-64 or WSL2 using Ubuntu 24.04')
    cmake,ninja,env,tools=build_tools(args)
    if args.stage=='check':
        print('Build tools are ready.');return 0
    if args.stage=='extractor':
        extractor(args,env,tools);return 0
    target=ROOT/'work/load_tu18'
    stems=['default'] if args.program=='campaign' else ['default_mp']+([] if args.without_campaign or args.program=='mp' else ['default'])
    hashes={}
    if args.inputs:
        selected=['default_mp'] if args.campaign_inputs and args.program!='campaign' else stems
        hashes.update(import_inputs(args.inputs.resolve(),target,selected))
    if args.campaign_inputs and not args.without_campaign:
        hashes.update(import_inputs(args.campaign_inputs.resolve(),target,['default']))
    if args.stage in ('native','game'):
        native(args,env,cmake,ninja,stems);return 0
    for stem in (['default_mp'] if args.stage=='apk' else stems):
        for extension in ('xex','xexp'):
            name=stem+'.'+extension
            if not (target/name).is_file():parser.error('Missing '+name+'; pass --inputs and, if necessary, --campaign-inputs')
            hashes[name]=digest(target/name)
    if args.stage=='inputs':return 0
    if args.stage in ('all','generate'):generate(args,hashes,env,cmake,ninja,stems)
    if args.stage in ('all','native'):native(args,env,cmake,ninja,stems)
    if args.stage in ('all','apk'):apk(args,env,tools,hashes,stems)
    return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,RuntimeError,OSError,subprocess.CalledProcessError,zipfile.BadZipFile) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
