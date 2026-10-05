param(
    [ValidateRange(1, 65535)][int]$Port = 8492,
    [string]$Snapshot = '',
    [switch]$NoBrowser,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$serverProcess = $null
$releaseRoot = $PSScriptRoot
$executable = Join-Path $releaseRoot 'nexus.exe'
$releaseData = Join-Path $releaseRoot 'data'
$releaseLogs = Join-Path $releaseRoot 'logs'
try {
    if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw 'nexus.exe is missing. Extract the entire portable ZIP before starting.'
    }
    if ($Snapshot -eq '') {
        [void][System.IO.Directory]::CreateDirectory($releaseData)
        $Snapshot = Join-Path $releaseData 'example.nxs'
        if (!(Test-Path -LiteralPath $Snapshot)) {
            & $executable build (Join-Path $releaseRoot 'examples\documents.jsonl') $Snapshot
            if ($LASTEXITCODE -ne 0) { throw 'Could not create the example snapshot.' }
        }
    }
    $Snapshot = (Resolve-Path -LiteralPath $Snapshot).Path
    & $executable stats $Snapshot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'The selected snapshot could not be validated.' }
    # Reserve-and-release detects a busy port before any browser is opened.
    # Another process can still win the race; the child liveness check catches it.
    $portProbe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    try { $portProbe.Start() } finally { $portProbe.Stop() }
    [void][System.IO.Directory]::CreateDirectory($releaseLogs)
    $logId = [System.Guid]::NewGuid().ToString('N')
    $stdoutLog = Join-Path $releaseLogs ("server-$logId.stdout.log")
    $stderrLog = Join-Path $releaseLogs ("server-$logId.stderr.log")
    # Windows paths cannot contain a literal quote. Quote the single path
    # argument explicitly because Windows PowerShell joins ArgumentList strings.
    $quotedSnapshot = '"' + $Snapshot + '"'
    $serverProcess = Start-Process -FilePath $executable -ArgumentList @('serve', $quotedSnapshot, '--host', '127.0.0.1', '--port', "$Port") -WorkingDirectory $releaseRoot -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
    $serverUrl = "http://127.0.0.1:$Port/"
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $serverProcess.Refresh()
        if ($serverProcess.HasExited) {
            $details = Get-Content -LiteralPath $stderrLog -Raw -ErrorAction SilentlyContinue
            throw "NexusSearch exited during startup. $details"
        }
        try {
            $health = Invoke-RestMethod -Uri ($serverUrl + 'health') -TimeoutSec 1 -UseBasicParsing
            $serverProcess.Refresh()
            if (!$serverProcess.HasExited -and $health.status -eq 'ok') { $ready = $true; break }
        } catch { }
        Start-Sleep -Milliseconds 100
    }
    if (!$ready) { throw 'The local server did not become ready within 15 seconds.' }
    Write-Host "NexusSearch is ready at $serverUrl"
    Write-Host "Snapshot: $Snapshot"
    if ($CheckOnly) {
        Write-Host 'Startup check passed.'
    } else {
        if (!$NoBrowser) { Start-Process $serverUrl }
        Write-Host 'Keep this window open. Press Enter to stop NexusSearch.'
        [void](Read-Host)
    }
} catch {
    Write-Host "NexusSearch could not start: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($null -ne $serverProcess) {
        $serverProcess.Refresh()
        if (!$serverProcess.HasExited) {
            Stop-Process -Id $serverProcess.Id -ErrorAction SilentlyContinue
            [void]$serverProcess.WaitForExit(5000)
        }
        $serverProcess.Dispose()
    }
}
