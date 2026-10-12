<#
.SYNOPSIS
    Build and test Lattice on Linux inside Docker, from Windows.

.DESCRIPTION
    Same options as scripts/linux-test.sh:
      --debug, --rtcheck, --rtsan, --clang, --tidy, --all-tests, --shell
    Needs Docker Desktop (WSL2 back end) running. Uses at most 4 cores and 4 GB of RAM.

.EXAMPLE
    scripts\linux-test.ps1
    scripts\linux-test.ps1 --rtsan
    scripts\linux-test.ps1 --clang --tidy
#>
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$image = 'lattice-linux:latest'

docker build --quiet -t $image -f (Join-Path $repoRoot 'docker/linux.Dockerfile') (Join-Path $repoRoot 'docker') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }

$run = @('run', '--rm', '--cpus', '4', '--memory', '4g')
# As in CI: set LATTICE_WARNINGS_AS_ERRORS=ON to fail on warnings.
if ($env:LATTICE_WARNINGS_AS_ERRORS) { $run += @('-e', "LATTICE_WARNINGS_AS_ERRORS=$($env:LATTICE_WARNINGS_AS_ERRORS)") }
# ThreadSanitizer re-executes itself with address randomisation off, which Docker's default
# seccomp profile blocks; only the TSan run relaxes it.
if ($args -contains '--tsan') { $run += @('--security-opt', 'seccomp=unconfined') }
$run += @(
         '-v', "${repoRoot}:/src",
         '-v', 'lattice-cache:/cache',
         '-v', 'lattice-build:/build')

if ($args -contains '--shell') {
    docker @run -it $image bash
} else {
    docker @run $image bash /src/scripts/linux-test.sh --inside @args
}
exit $LASTEXITCODE
