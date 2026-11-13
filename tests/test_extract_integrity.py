# Fatal metadata integrity failures must stop with a useful error, while existing file-level handling remains intact.
import os, shutil, subprocess, sys, tempfile, unittest, xml.etree.ElementTree as ET
from pathlib import Path
from xml.sax.saxutils import escape

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from porter.toolchain import dotnet_sdks

PROJECT = REPO / 'nxnative/dm/tools/nx_extract/nx_extract.csproj'


class ExtractIntegrity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dotnet = shutil.which('dotnet')
        if not cls.dotnet and os.environ.get('DOTNET_ROOT'):
            candidate = Path(os.environ['DOTNET_ROOT']) / ('dotnet.exe' if os.name == 'nt' else 'dotnet')
            if candidate.is_file(): cls.dotnet = str(candidate)
        project = ET.parse(PROJECT).getroot()
        target = project.findtext('./PropertyGroup/TargetFramework')
        major = int(target.removeprefix('net').split('.')[0])
        sdks = dotnet_sdks(cls.dotnet) if cls.dotnet else {}
        matching = [Path(root).parent for version, root in sdks.items() if int(version.split('.')[0]) == major]
        if not matching:
            raise unittest.SkipTest(f'.NET SDK {major} is required')
        if not any(list((root / 'packs/Microsoft.NETCore.App.Ref').glob(f'{major}.*/*/{target}/System.Runtime.dll')) for root in matching):
            raise unittest.SkipTest(f'.NET {major} reference assemblies are required')
        runtimes = subprocess.run([str(cls.dotnet), '--list-runtimes'], capture_output=True, text=True, timeout=30)
        if not any(line.startswith(f'Microsoft.NETCore.App {major}.') for line in runtimes.stdout.splitlines()):
            raise unittest.SkipTest(f'.NET {major} runtime is required')
        version = next(p.attrib['Version'] for p in project.findall('./ItemGroup/PackageReference') if p.attrib['Include'] == 'LibHac')
        packages = Path(os.environ.get('NUGET_PACKAGES', Path.home() / '.nuget/packages'))
        dlls = sorted((packages / 'libhac' / version / 'lib').glob('*/LibHac.dll'))
        if not dlls:
            raise unittest.SkipTest(f'LibHac {version} is not cached; build nx_extract first')
        cls.folder = tempfile.TemporaryDirectory(prefix='nx2a_extract_integrity_')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.work = Path(cls.folder.name)
        cls.env = dict(os.environ, DOTNET_CLI_HOME=str(cls.work / 'cli'), DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
                       DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true')
        (cls.work / 'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
        # Compile the full production source with the exact cached LibHac version; never fetch or install test dependencies.
        (cls.work / 'Fixture.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>{target}</TargetFramework><StartupObject>IntegrityFixture</StartupObject>
<AllowUnsafeBlocks>true</AllowUnsafeBlocks><EnableDefaultCompileItems>false</EnableDefaultCompileItems><NuGetAudit>false</NuGetAudit>
</PropertyGroup><ItemGroup><Compile Include="{escape(str(PROJECT.with_name('Program.cs')))}" />
<Compile Include="{escape(str(REPO / 'tests/nx_extract_integrity_fixture.cs'))}" />
<Reference Include="LibHac"><HintPath>{escape(str(dlls[-1]))}</HintPath></Reference></ItemGroup></Project>''')
        built = subprocess.run([str(cls.dotnet), 'build', str(cls.work / 'Fixture.csproj'), '--nologo', '-v:q'],
                               cwd=cls.work, env=cls.env, text=True, capture_output=True, timeout=120)
        if built.returncode: raise AssertionError(built.stdout + built.stderr)
        cls.fixture = cls.work / 'bin/Debug' / target / 'Fixture.dll'

    def run_case(self, mode):
        out = self.work / mode
        result = subprocess.run([str(self.dotnet), str(self.fixture), mode, str(out)], env=self.env, text=True, capture_output=True, timeout=30)
        return result, out

    def test_fatal_metadata_integrity_is_reported_even_with_allow_damaged(self):
        for mode in ('fatal', 'fatal-allow'):
            with self.subTest(mode=mode):
                r, out = self.run_case(mode)
                self.assertEqual(r.returncode, 3, r.stderr)
                self.assertIn('[RED]', r.stderr)
                self.assertIn('2002-4604', r.stderr)
                self.assertIn('Hash error', r.stderr)
                self.assertNotIn('UNHANDLED', r.stderr)
                self.assertFalse(out.exists())

    def test_nca_header_failure_names_input_and_entry_even_with_allow_damaged(self):
        for mode in ('nca-bad', 'nca-allow'):
            with self.subTest(mode=mode):
                r, out = self.run_case(mode)
                self.assertEqual(r.returncode, 4, r.stderr)
                self.assertIn('[RED]', r.stderr)
                self.assertIn(str(self.work / (mode + " container's sample.nsp")), r.stderr)
                self.assertIn('header-entry.nca', r.stderr)
                self.assertIn('Unable to decrypt NCA header.', r.stderr)
                self.assertNotIn('UNHANDLED', r.stderr)
                self.assertNotIn('filesystem integrity check', r.stderr)
                self.assertFalse(out.exists())

    def test_readable_nca_header_is_still_opened(self):
        r, _ = self.run_case('nca-readable')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, '')

    def test_unrelated_exceptions_are_not_reclassified(self):
        for mode, kind in (('invalid-data', 'System.IO.InvalidDataException'), ('io', 'System.IO.IOException'), ('other-result', 'HorizonResultException'),
                           ('other-module', 'HorizonResultException'), ('outside-range', 'HorizonResultException')):
            with self.subTest(mode=mode):
                r, _ = self.run_case(mode)
                self.assertEqual(r.returncode, 91)
                self.assertIn('UNHANDLED', r.stderr)
                self.assertIn(kind, r.stderr)
                self.assertNotIn('[RED]', r.stderr)

    def test_success_and_usage_statuses_are_preserved(self):
        r, out = self.run_case('copy')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((out / 'sample.bin').read_bytes(), bytes((i * 17 + 3) & 255 for i in range(8192)))
        r, _ = self.run_case('usage')
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn('usage: nx_extract', r.stderr)

    def test_file_data_integrity_keeps_existing_allow_damaged_behavior(self):
        r, out = self.run_case('copy-bad')
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn('hash check failed: /sample.bin', r.stdout)
        self.assertFalse((out / 'sample.bin').exists())
        r, out = self.run_case('copy-allow')
        self.assertEqual(r.returncode, 0, r.stderr)
        expected = bytearray((i * 17 + 3) & 255 for i in range(8192)); expected[4096] ^= 1
        self.assertEqual((out / 'sample.bin').read_bytes(), expected)
        self.assertIn('--allow-damaged', r.stdout)


if __name__ == '__main__': unittest.main()
