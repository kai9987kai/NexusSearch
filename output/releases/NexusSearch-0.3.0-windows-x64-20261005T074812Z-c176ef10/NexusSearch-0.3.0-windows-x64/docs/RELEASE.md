# NexusSearch 0.3.0 portable release

This release workflow targets a local Windows x64 installation. It does not
install files into Windows, require administrator access, bind outside loopback,
or publish a service. The package uses the x86-64-v2 instruction baseline and
must run on a compatible x64 processor; it is not a native ARM64 build.

Run the release builder from the project root on Windows with Python 3, CMake,
Ninja, MSYS2 UCRT64 GCC and `objdump` available:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
python tools\build_release.py --output-dir output\releases
```

The builder freezes a source copy, configures a clean optimized build, runs
CTest, inspects executable DLL imports, bundles non-Windows runtime DLLs, and
creates a package ZIP plus SHA-256 file. It then copies the package to a path
with spaces and Unicode and exercises the CLI, prepared text query, Python DLL
loading, HTTP health/search, launcher startup and preservation of an existing
example snapshot. `VALIDATION.json` records the actual tested machine and
runtime. `RELEASE_MANIFEST.json` covers package files; `SOURCE_MANIFEST.json`
records hashes of the bundled source tree; `DEPENDENCIES.json` records imported
DLL resolution. These hashes detect changes and transfer damage but are not a
digital signature or publisher identity.

To check an extracted package manifest without building it:

```powershell
python tools\build_release.py --verify output\releases\<release>\NexusSearch-0.3.0-windows-x64
```

Extract the entire ZIP before launch. `Start NexusSearch.cmd` creates the sample
snapshot once, starts the loopback-only server, waits for its health response,
then opens the browser. Keep its window open to keep the server running. Use
`Start-NexusSearch.ps1 -Snapshot <path>` to choose another snapshot, or
`-CheckOnly -NoBrowser` to run a launch smoke check.

The package is unsigned. The project had no license file at release preparation;
the package does not grant rights to redistribute project source or bundled
dependencies. The manifest lists bundled files and the dependency report lists
runtime DLLs; check upstream license terms before any redistribution. The source
copy is included for audit and local rebuilds, not as a license statement.

The atomic single-file snapshot replacement flushes file contents, but filesystem
and OS metadata durability vary. Snapshot updates require cooperating writers to
use the same path spelling and persistent `.lock` sidecar. This release makes no
universal power-loss guarantee. It does not include ANN or embedding inference;
vector ranking is exact cosine over each eligible stored vector. The CI matrix
is a repeatable validation configuration, not evidence until its hosted jobs
have actually run.
