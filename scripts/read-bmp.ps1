param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [ValidateSet("png", "jpg", "jpeg")]
    [string]$Format = "png",

    [string]$OutputPath,

    [switch]$Upload,

    [string]$SessionId = $env:KIMAKI_SESSION_ID
)

$ErrorActionPreference = "Stop"

$resolvedInput = Resolve-Path -LiteralPath $InputPath -ErrorAction Stop
$inputFullPath = $resolvedInput.Path

$inputExt = [System.IO.Path]::GetExtension($inputFullPath).ToLowerInvariant()
if ($inputExt -ne ".bmp") {
    Write-Warning "Input is '$inputExt', not .bmp. Continuing conversion anyway."
}

$normalizedFormat = $Format.ToLowerInvariant()
if ($normalizedFormat -eq "jpeg") {
    $normalizedFormat = "jpg"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)
    $dirName = [System.IO.Path]::GetDirectoryName($inputFullPath)
    $OutputPath = Join-Path $dirName ("{0}.{1}" -f $baseName, $normalizedFormat)
}

$magick = Get-Command magick -ErrorAction SilentlyContinue
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}

if ($magick) {
    & $magick.Source $inputFullPath $OutputPath
} elseif ($ffmpeg) {
    & $ffmpeg.Source -y -i $inputFullPath $OutputPath
} else {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($inputFullPath)
    try {
        if ($normalizedFormat -eq "jpg") {
            $image.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Jpeg)
        } else {
            $image.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
        }
    } finally {
        $image.Dispose()
    }
}

$resolvedOutput = Resolve-Path -LiteralPath $OutputPath -ErrorAction Stop
$outputFullPath = $resolvedOutput.Path

Write-Host "Converted: $inputFullPath"
Write-Host "Output: $outputFullPath"

if ($Upload) {
    if ([string]::IsNullOrWhiteSpace($SessionId)) {
        throw "Upload requested but SessionId is empty. Pass -SessionId <session> or set KIMAKI_SESSION_ID."
    }

    Write-Host "Uploading to Discord via kimaki session: $SessionId"
    & npx -y kimaki upload-to-discord --session $SessionId $outputFullPath
}

# Keep final line machine-friendly for piping.
Write-Output $outputFullPath
