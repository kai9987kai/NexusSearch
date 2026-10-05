NexusSearch portable for Windows
===============================

Extract the whole ZIP into a writable folder, including one whose name contains
spaces. Double-click "Start NexusSearch.cmd". Your browser opens only after the
local server is ready. Keep the console open; press Enter to stop the server.
Nothing is installed and no administrator permissions are required.

The first launch builds data\example.nxs from examples\documents.jsonl.
Later launches keep that file. Existing snapshots and documents are never reset.
Startup diagnostics are stored in logs\ with a new filename on each launch.

The server binds only to 127.0.0.1, using port 8492. If that port is occupied,
choose another port from PowerShell:
  .\Start-NexusSearch.ps1 -Port 8493
To open your own snapshot:
  .\Start-NexusSearch.ps1 -Snapshot 'C:\My documents\catalog.nxs'
PowerShell execution policy may require invoking powershell.exe with
-NoProfile -ExecutionPolicy Bypass -File before the script path; the supplied
CMD launcher sets that process-only option. It does not change system policy.

Command-line use (PowerShell, in this folder):
  .\nexus.exe --help
  .\nexus.exe build examples\documents.jsonl data\my-example.nxs
  .\nexus.exe search data\my-example.nxs 'search AND active:true'
  .\nexus.exe search data\my-example.nxs 'title:prefix(sea)' --prepared
  .\nexus.exe update data\my-example.nxs updates.jsonl

Prepared exact text search is built in memory and discarded when the command or
server exits. The HTTP server builds it once at startup. Regex, fuzzy, very short
substring and all vector queries retain their exact reference paths.

Python is optional and is not bundled. If Python 3.10+ is installed, use:
  $env:PYTHONPATH = "$PWD\bindings\python"
  $env:NEXUS_LIB_PATH = "$PWD\libnexus.dll"
Then import Snapshot from nexus. See bindings\python\README.md.

RELEASE_MANIFEST.json lists hashes for every supplied file. SOURCE_MANIFEST.json
identifies the source snapshot used for the build. The included source\ folder
contains that exact source. These hashes detect accidental changes; the package
is unsigned and the hashes are not a signature or publisher authentication.

This build targets Windows x64, with x86-64-v2 instructions. The validation
receipt records the actual tested host; other Windows versions and processors
are not implied to have been tested. Windows ARM64 may run this x64 build through
emulation; there is no native ARM64 executable in this package.

Only Windows system DLLs and dependencies recorded in DEPENDENCIES.json are
required. No MSYS2 development installation is needed to run the package.

This is a local portable release. No project license file was present when the
packaging workflow was introduced. Redistribution rights for project sources
remain unresolved; do not treat this package as granting a license. Dependency
imports and any bundled runtime DLLs are listed in DEPENDENCIES.json. Review
their upstream licenses before redistributing. No code-signing or public
deployment is included. See docs\RELEASE.md for verification and limitations.
