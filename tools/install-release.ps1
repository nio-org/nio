# Install the nio compiler from a GitHub release (Windows).
#
#   irm https://github.com/nio-org/nio/releases/latest/download/install.ps1 | iex
#
# The release publishes this file as install.ps1. It downloads the archive,
# checks it against the release's SHA256SUMS, puts nio.exe in $NIO_INSTALL,
# and adds that directory to the user's PATH.
#
# Environment:
#   NIO_VERSION       the release to install, e.g. v0.1.0   (default: the latest)
#   NIO_INSTALL       the directory to put nio.exe in        (default: %LOCALAPPDATA%\Programs\nio)
#   NIO_REPO          the GitHub repository of the releases  (default: nio-org/nio)
#   NIO_DOWNLOAD_URL  a directory holding the release files; replaces the
#                     two above, for a mirror
#
# macOS and Linux use tools/install-release.sh, published as install.sh.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Fail($message) {
    Write-Host "nio install: $message" -ForegroundColor Red
    throw "nio install failed"
}

$repo = if ($env:NIO_REPO) { $env:NIO_REPO } else { 'nio-org/nio' }
$version = if ($env:NIO_VERSION) { $env:NIO_VERSION } else { 'latest' }
$dest = if ($env:NIO_INSTALL) { $env:NIO_INSTALL } else { Join-Path $env:LOCALAPPDATA 'Programs\nio' }

if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
    Fail "no prebuilt nio for $($env:PROCESSOR_ARCHITECTURE); build it from source: https://nio-lang.org/docs/installation"
}
$asset = 'nio-windows-x86_64.zip'

if ($env:NIO_DOWNLOAD_URL) {
    $base = $env:NIO_DOWNLOAD_URL
} elseif ($version -eq 'latest') {
    $base = "https://github.com/$repo/releases/latest/download"
} else {
    $base = "https://github.com/$repo/releases/download/$version"
}

# Windows PowerShell 5.1 does not offer TLS 1.2 by default, and GitHub
# accepts nothing older.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("nio-install-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp $asset
    $sums = Join-Path $tmp 'SHA256SUMS'
    Write-Host "downloading $base/$asset"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $zip
        Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
    } catch {
        Fail "cannot download from ${base}: $($_.Exception.Message)"
    }

    $expected = $null
    foreach ($line in Get-Content $sums) {
        $fields = $line -split '\s+'
        if ($fields.Count -ge 2 -and $fields[1] -eq $asset) {
            $expected = $fields[0].ToLower()
        }
    }
    if (-not $expected) {
        Fail "SHA256SUMS has no entry for $asset"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
    if ($expected -ne $actual) {
        Fail "checksum mismatch for ${asset}: expected $expected, got $actual"
    }

    $unpacked = Join-Path $tmp 'unpacked'
    Expand-Archive -Path $zip -DestinationPath $unpacked
    $exe = Join-Path $unpacked 'nio.exe'
    if (-not (Test-Path $exe)) {
        Fail "$asset holds no nio.exe"
    }

    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $target = Join-Path $dest 'nio.exe'
    try {
        Move-Item -Force -Path $exe -Destination $target
    } catch {
        Fail "cannot replace ${target}; close any running nio (an editor's language server, for example) and run this again"
    }
} finally {
    Remove-Item -Recurse -Force -Path $tmp -ErrorAction SilentlyContinue
}

Write-Host "installed $(& $target version) at $target"

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = if ($userPath) { $userPath -split ';' } else { @() }
if ($entries -notcontains $dest) {
    $newPath = if ($userPath) { "$dest;$userPath" } else { $dest }
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    $env:Path = "$dest;$env:Path"
    Write-Host "added $dest to your PATH; terminals opened from now on will find nio"
}

# nio compiles through clang, so a machine without it can install nio and
# cannot build anything.
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
    Write-Host ''
    Write-Host 'nio needs clang on PATH to build programs, and there is none. Install:'
    Write-Host '  LLVM:  winget install LLVM.LLVM   (then add C:\Program Files\LLVM\bin to PATH)'
    Write-Host '  the Visual Studio Build Tools with the "Desktop development with C++" workload,'
    Write-Host '  which is where clang finds the C runtime.'
}
