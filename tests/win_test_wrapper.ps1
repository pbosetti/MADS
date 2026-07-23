# Windows-only helper invoked as Catch2's CROSSCOMPILING_EMULATOR (see
# CMakeLists.txt in this directory). Not actually about cross-compiling: it's
# the only hook Catch2 honors for both its build-time/ctest-time test
# *discovery* run and the later real test run, so it's repurposed here purely
# to fix up PATH before running the test executable.
#
# A system-wide MADS install can leave an old MadsCore.dll on the machine's
# PATH; since build\tests\ never gets its own copy, the Windows loader would
# otherwise find that stale DLL first and fail with STATUS_ENTRYPOINT_NOT_FOUND.
# This script prepends the freshly built DLL's directory to PATH for the
# child process only -- it never touches the real system/session PATH.
#
# A plain "cmake -P" script (the first version of this wrapper) can't do this:
# CROSSCOMPILING_EMULATOR is a CMake list, PATH is ';'-separated on Windows,
# and CMake's own list separator is also ';', so passing a full PATH value
# through it across the several -D VAR=... hops inside Catch2's own CMake
# scripts corrupts it -- hence doing the PATH edit here, at run time, instead
# of baking it into the property. Just as important, "cmake -P" has no way to
# exit with an arbitrary status code (only fixed/generic ones), which broke
# Catch2's SKIP_RETURN_CODE convention (exit code 4 means "skipped", not
# "failed"); PowerShell's exit propagates the child's real code exactly.
#
# Usage: win_test_wrapper.ps1 <dll_dir> <exe> [args...]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$DllDir,

  [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
  [string[]]$Rest
)

$env:PATH = "$DllDir;$env:PATH"

$exe = $Rest[0]
$exeArgs = @()
if ($Rest.Count -gt 1) {
  $exeArgs = $Rest[1..($Rest.Count - 1)]
}

& $exe @exeArgs
exit $LASTEXITCODE
