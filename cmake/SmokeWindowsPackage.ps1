param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$extract = Join-Path $evidence ('portable-' + [Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $archivePath -DestinationPath $extract
$candidates = @(Get-ChildItem -LiteralPath $extract -Filter logi-native.exe -Recurse -File)
if ($candidates.Count -ne 1) { throw 'Expected one packaged GUI executable' }
$package = $candidates[0].DirectoryName
$cleanPath = "$env:SystemRoot\System32;$env:SystemRoot"
$results = @()

function Invoke-PackagedCommand([string]$Name, [string]$Program, [string[]]$Arguments, [string]$Platform) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $package $Program
    $start.WorkingDirectory = $package
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.Environment['PATH'] = $cleanPath
    foreach ($key in @('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QTDIR', 'QT_DIR')) {
        $start.Environment.Remove($key) | Out-Null
    }
    $start.Environment['QT_QPA_PLATFORM'] = $Platform
    $start.Environment['LOGI_OPTIONS_OPEN_CONFIG'] = Join-Path $extract 'unused-test-config.ini'
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "$Name failed to launch" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "$Name timed out"
        }
        # Explicit process wait is required for a Windows GUI subsystem EXE.
        # PowerShell's call operator alone can report success before it exits.
        $out = $stdout.GetAwaiter().GetResult()
        $err = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $evidence "$Name.stdout.log"), $out)
        [IO.File]::WriteAllText((Join-Path $evidence "$Name.stderr.log"), $err)
        $result = [pscustomobject]@{name=$Name; exit_code=$process.ExitCode; path=$cleanPath; qt_platform=$Platform}
        $script:results += $result
        $script:results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $evidence 'portable-smoke.json')
        if ($process.ExitCode -ne 0) { throw "$Name exited $($process.ExitCode): $err" }
        Write-Host "PASS $Name (clean PATH, exit 0)"
    } finally { $process.Dispose() }
}

Invoke-PackagedCommand 'portable-windows-plugin' 'logi-native.exe' @('--self-test') 'windows'
Invoke-PackagedCommand 'portable-cli-config' 'logi-cli.exe' @('check-config', (Join-Path $package 'mappings.ini')) 'windows'
if (-not (Test-Path -LiteralPath (Join-Path $package 'licenses/runtime-packages.txt'))) {
    throw 'Packaged runtime provenance/licenses manifest is missing'
}
Write-Host 'Extracted portable package passed without Qt/MinGW directories in PATH. No hardware commands were run.'
