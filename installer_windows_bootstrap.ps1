$RepoUrl = "https://github.com/Valephnull/RFF-EXP.git"

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($InstallRoot))
{
    $InstallRoot = (Get-Location).Path
}

$VersionFile = Join-Path $InstallRoot "version.config"
$SourceDir = Join-Path $InstallRoot ".rff-exp-source"
$StagingDir = Join-Path $InstallRoot ".rff-exp-installing"
$BackupDir = Join-Path $InstallRoot ".rff-exp-backup"
$TargetDirs = @("res", "bin", "shaders")

function Write-Step([string]$Message)
{
    Write-Host ""
    Write-Host "==== $Message ====" -ForegroundColor Cyan
}

function Assert-NativeSuccess([string]$Action)
{
    if ($LASTEXITCODE -ne 0)
    {
        throw "$Action failed with exit code $LASTEXITCODE."
    }
}

function Remove-DirectoryIfPresent([string]$Path)
{
    if (Test-Path -LiteralPath $Path)
    {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function ConvertTo-MsysPath([string]$Bash, [string]$Path)
{
    $Lines = @(& $Bash -lc 'cygpath -u "$1"' -- $Path)
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0)
    {
        throw "Converting '$Path' to an MSYS2 path failed with exit code $ExitCode."
    }

    $Converted = $Lines | Select-Object -Last 1
    if ([string]::IsNullOrWhiteSpace($Converted))
    {
        throw "MSYS2 did not return a path for '$Path'."
    }
    return $Converted.Trim()
}

function Get-RemoteHeadSha([string]$Bash, [string]$Url)
{
    # MSYS2 Git must run inside its Bash environment. Launching usr\bin\git.exe
    # directly from PowerShell can abort its HTTPS helper and report exit code -1.
    $Lines = @(& $Bash -lc 'git ls-remote --exit-code "$1" HEAD' -- $Url)
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0)
    {
        throw "Reading the repository version failed with exit code $ExitCode. Check the GitHub connection and try again."
    }

    $Line = $Lines | Where-Object { $_ -match '^[0-9a-fA-F]{40}\s+HEAD$' } | Select-Object -First 1
    if (-not $Line)
    {
        throw "The repository did not return a HEAD revision: $Url"
    }
    return ($Line -split "\s+")[0]
}

$IsAdministrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)
if (-not $IsAdministrator)
{
    throw "Run installer_windows.bat as Administrator."
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue))
{
    throw "winget was not found. Install 'App Installer' from the Microsoft Store, then run this installer again."
}

Write-Step "MSYS2 installation"

$Msys2Root = "C:\msys64"
if ($env:MSYS2_ROOT -and (Test-Path -LiteralPath (Join-Path $env:MSYS2_ROOT "usr\bin\bash.exe")))
{
    $Msys2Root = $env:MSYS2_ROOT
}

$Msys2Bash = Join-Path $Msys2Root "usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $Msys2Bash))
{
    winget install --exact --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements --silent
    Assert-NativeSuccess "Installing MSYS2"
}
if (-not (Test-Path -LiteralPath $Msys2Bash))
{
    throw "MSYS2 installation did not create $Msys2Bash."
}

Write-Step "MSYS2 package update and installation"

& $Msys2Bash -lc "pacman -Syu --noconfirm --needed"
Assert-NativeSuccess "Updating MSYS2"
& $Msys2Bash -lc "pacman -Syu --noconfirm --needed"
Assert-NativeSuccess "Completing the MSYS2 update"

$Packages = @(
    "git",
    "mingw-w64-clang-x86_64-clang",
    "mingw-w64-clang-x86_64-make",
    "mingw-w64-clang-x86_64-cmake",
    "mingw-w64-clang-x86_64-ninja",
    "mingw-w64-clang-x86_64-gmp",
    "mingw-w64-clang-x86_64-vulkan",
    "mingw-w64-clang-x86_64-glm",
    "mingw-w64-clang-x86_64-glfw",
    "mingw-w64-clang-x86_64-opencv",
    "mingw-w64-clang-x86_64-openssl",
    "mingw-w64-clang-x86_64-shaderc"
) -join " "

& $Msys2Bash -lc "pacman -S --noconfirm --needed $Packages"
Assert-NativeSuccess "Installing the RFF build dependencies"

Write-Step "Configuring the build environment"

$Clang64Bin = Join-Path $Msys2Root "clang64\bin"
$UsrBin = Join-Path $Msys2Root "usr\bin"
$env:MSYS2_ROOT = $Msys2Root
$env:PATH = "$Clang64Bin;$UsrBin;$env:PATH"

# RFF and OpenCV depend on DLLs in clang64\bin. Persist this path so RFF also
# starts after the installer process has closed.
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$UserPathParts = @($UserPath -split ";" | Where-Object { $_ })
if ($UserPathParts -notcontains $Clang64Bin)
{
    $UpdatedUserPath = (@($Clang64Bin) + $UserPathParts) -join ";"
    [Environment]::SetEnvironmentVariable("Path", $UpdatedUserPath, "User")
}

Write-Step "Checking the installed version"

$SourceDirUnix = ConvertTo-MsysPath $Msys2Bash $SourceDir
$NewestSha = Get-RemoteHeadSha $Msys2Bash $RepoUrl
$InstalledSha = if (Test-Path -LiteralPath $VersionFile)
{
    (Get-Content -LiteralPath $VersionFile -Raw).Trim()
}
else
{
    ""
}

$InstalledExecutable = Join-Path $InstallRoot "bin\RFF.exe"
$InstallComplete = Test-Path -LiteralPath $InstalledExecutable
foreach ($Name in $TargetDirs)
{
    $InstallComplete = $InstallComplete -and (Test-Path -LiteralPath (Join-Path $InstallRoot $Name))
}
if ($InstalledSha -eq $NewestSha -and $InstallComplete)
{
    Write-Host "RFF-EXP is already up to date ($($NewestSha.Substring(0, 7)))." -ForegroundColor Green
    Read-Host "Press Enter to exit"
    exit 0
}

Write-Step "Downloading RFF-EXP"

Remove-DirectoryIfPresent $SourceDir
& $Msys2Bash -lc 'git clone --depth 1 "$1" "$2"' -- $RepoUrl $SourceDirUnix
Assert-NativeSuccess "Cloning RFF-EXP"

Write-Step "Downloading external source dependencies"

$ExternDir = Join-Path $SourceDir "extern"
New-Item -ItemType Directory -Path $ExternDir -Force | Out-Null
$ExternSourcesFile = Join-Path $SourceDir "extern_sources"
$ExternRepos = Get-Content -LiteralPath $ExternSourcesFile |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and -not $_.StartsWith("#") }

foreach ($Url in $ExternRepos)
{
    $DirectoryName = [System.IO.Path]::GetFileNameWithoutExtension($Url.TrimEnd('/'))
    $Destination = Join-Path $ExternDir $DirectoryName
    $DestinationUnix = ConvertTo-MsysPath $Msys2Bash $Destination
    & $Msys2Bash -lc 'git clone --depth 1 "$1" "$2"' -- $Url $DestinationUnix
    Assert-NativeSuccess "Cloning external dependency $Url"
}

Write-Step "Building RFF-EXP"

$BuildDir = Join-Path $SourceDir "build"
cmake -S $SourceDir -B $BuildDir -G "Ninja" `
    -DCMAKE_C_COMPILER=clang `
    -DCMAKE_CXX_COMPILER=clang++ `
    -DCMAKE_BUILD_TYPE=Release
Assert-NativeSuccess "Configuring RFF-EXP"

$ParallelJobs = if ($env:NUMBER_OF_PROCESSORS) { [int]$env:NUMBER_OF_PROCESSORS } else { 1 }
cmake --build $BuildDir --parallel $ParallelJobs
Assert-NativeSuccess "Building RFF-EXP"

& $Msys2Bash -lc 'export PATH="/clang64/bin:$PATH"; cd "$1" && ./compile.sh' -- $SourceDirUnix
Assert-NativeSuccess "Compiling RFF-EXP shaders"

Write-Step "Installing RFF-EXP"

Remove-DirectoryIfPresent $StagingDir
Remove-DirectoryIfPresent $BackupDir
New-Item -ItemType Directory -Path $StagingDir | Out-Null
New-Item -ItemType Directory -Path $BackupDir | Out-Null

foreach ($Name in $TargetDirs)
{
    $Source = Join-Path $SourceDir $Name
    if (-not (Test-Path -LiteralPath $Source))
    {
        throw "The completed build is missing '$Name'."
    }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $StagingDir $Name) -Recurse
}

$BackedUp = [System.Collections.Generic.List[string]]::new()
$Installed = [System.Collections.Generic.List[string]]::new()
try
{
    foreach ($Name in $TargetDirs)
    {
        $Destination = Join-Path $InstallRoot $Name
        if (Test-Path -LiteralPath $Destination)
        {
            Move-Item -LiteralPath $Destination -Destination (Join-Path $BackupDir $Name)
            $BackedUp.Add($Name)
        }
    }

    foreach ($Name in $TargetDirs)
    {
        Move-Item -LiteralPath (Join-Path $StagingDir $Name) -Destination (Join-Path $InstallRoot $Name)
        $Installed.Add($Name)
    }
}
catch
{
    foreach ($Name in $Installed)
    {
        Remove-DirectoryIfPresent (Join-Path $InstallRoot $Name)
    }
    foreach ($Name in $BackedUp)
    {
        $Backup = Join-Path $BackupDir $Name
        if (Test-Path -LiteralPath $Backup)
        {
            Move-Item -LiteralPath $Backup -Destination (Join-Path $InstallRoot $Name)
        }
    }
    throw
}

$VersionTemp = "$VersionFile.tmp"
Set-Content -LiteralPath $VersionTemp -Value $NewestSha -NoNewline
Move-Item -LiteralPath $VersionTemp -Destination $VersionFile -Force

Remove-DirectoryIfPresent $BackupDir
Remove-DirectoryIfPresent $StagingDir
Remove-DirectoryIfPresent $SourceDir

Write-Step "Installation finished"
Write-Host "Location: $InstallRoot" -ForegroundColor Green
Write-Host "Version:  $($NewestSha.Substring(0, 7))" -ForegroundColor Green

$Launch = Read-Host "Launch RFF-EXP now? [y/N]"
if ($Launch -match "^[Yy]")
{
    Start-Process -FilePath $InstalledExecutable -WorkingDirectory (Join-Path $InstallRoot "bin")
}
