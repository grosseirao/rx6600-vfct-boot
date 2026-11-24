#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string] $Image = (Join-Path $PSScriptRoot '..\build\Rx6600DriverPerfil.efi')
)

$ErrorActionPreference = 'Stop'
$source = Get-Item -LiteralPath $Image
if ($source.PSIsContainer -or $source.Name -ne 'Rx6600DriverPerfil.efi') {
    throw 'Select the locally built Rx6600DriverPerfil.efi.'
}
$expectedHash = (Get-FileHash -LiteralPath $source.FullName).Hash

$partitions = @(Get-Partition | Where-Object {
    $_.IsSystem -and $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
})
if ($partitions.Count -ne 1) { throw 'Could not identify the Windows EFI system partition.' }
$expectedVolume = @($partitions[0].AccessPaths | Where-Object { $_ -like '\\?\Volume{*}\' })
if ($expectedVolume.Count -ne 1) { throw 'Could not identify the EFI volume path.' }

$letter = 'R','S','T','U','V','W','Y','Z' | Where-Object {
    !(Get-PSDrive -Name $_ -ErrorAction SilentlyContinue)
} | Select-Object -First 1
if (!$letter) { throw 'No temporary drive letter is available.' }
$mount = $letter + ':'
$mountvol = Join-Path $env:SystemRoot 'System32\mountvol.exe'
& $mountvol $mount /S
if ($LASTEXITCODE -ne 0) { throw 'Could not mount the Windows EFI system partition.' }

try {
    $volume = (& $mountvol $mount /L | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $volume -ne $expectedVolume[0]) { throw 'Unexpected EFI volume.' }

    $root = "$mount\"
    $loader = Join-Path $root 'EFI\Microsoft\Boot\bootmgfw.efi'
    if (!(Test-Path -LiteralPath $loader -PathType Leaf)) { throw 'Windows Boot Manager is missing.' }
    $directory = Join-Path $root 'EFI\RX6600-Test'
    $target = Join-Path $directory $source.Name
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target).Hash -ne $expectedHash) {
            throw 'A different EFI file is already installed. The existing file was preserved.'
        }
    } else {
        if (!(Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -Path $directory -ItemType Directory | Out-Null
        }
        $temporary = Join-Path $directory ("{0}.tmp" -f [guid]::NewGuid())
        try {
            Copy-Item -LiteralPath $source.FullName -Destination $temporary
            if ((Get-FileHash -LiteralPath $temporary).Hash -ne $expectedHash) {
                throw 'EFI copy failed hash verification.'
            }
            Move-Item -LiteralPath $temporary -Destination $target
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
        }
    }
    Write-Output "\EFI\RX6600-Test\$($source.Name) verified (SHA-256 $expectedHash)."
} finally {
    & $mountvol $mount /D
    if ($LASTEXITCODE -ne 0) { throw "Could not remove the temporary drive letter $mount." }
}
