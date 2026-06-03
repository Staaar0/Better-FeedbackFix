# vim: set sts=2 ts=8 sw=2 tw=99 et:
import sys

try:
    from ambuild2 import run
except Exception:
    try:
        import ambuild  # noqa: F401
        sys.stderr.write('It looks like AMBuild 1 is installed, but this project uses AMBuild 2.\n')
        sys.stderr.write('Upgrade to the latest AMBuild to continue.\n')
    except Exception:
        sys.stderr.write('AMBuild must be installed to build this project.\n')
        sys.stderr.write('https://github.com/alliedmodders/ambuild\n')
    sys.exit(1)

ambuild_version = getattr(run, 'CURRENT_API', '2.1')
if ambuild_version.startswith('2.1'):
    sys.stderr.write('AMBuild 2.2 or higher is required; please update.\n')
    sys.exit(1)

parser = run.BuildParser(sourcePath=sys.path[0], api='2.2')
parser.options.add_argument('--hl2sdk-root', type=str, dest='hl2sdk_root', default=None,
                            help='Root folder containing hl2sdk-* folders')
parser.options.add_argument('--hl2sdk-manifests', type=str, dest='hl2sdk_manifests', default=None,
                            help='Path to hl2sdk-manifests')
parser.options.add_argument('--mms_path', type=str, dest='mms_path', default=None,
                            help='Path to Metamod:Source source tree')
parser.options.add_argument('--enable-debug', action='store_const', const='1', dest='debug',
                            help='Enable debugging symbols')
parser.options.add_argument('--enable-optimize', action='store_const', const='1', dest='opt',
                            help='Enable optimization')
parser.options.add_argument('-s', '--sdks', default='cs2', dest='sdks',
                            help='SDKs to build; use cs2 for Counter-Strike 2')
parser.options.add_argument('--targets', type=str, dest='targets', default='x86_64',
                            help='Target architecture; CS2 uses x86_64')
parser.Configure()
