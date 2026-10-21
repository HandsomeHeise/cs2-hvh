param([switch]$SkipRemote, [switch]$UnitOnly, [string]$Dist = 'dist')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskSourceDist = [System.IO.Path]::GetFullPath((Join-Path $taskRoot $Dist))
$taskDist = Join-Path $taskRoot 'build\test-runtime'
$taskResults = Join-Path $taskRoot 'build\test-results'
New-Item -ItemType Directory -Force -Path $taskResults | Out-Null
New-Item -ItemType Directory -Force -Path $taskDist | Out-Null
foreach ($taskBinary in @('hvh_menu.dll','hvh_loader.exe','dx11_test_host.exe','feature_tests.exe','skin_transaction_tests.exe','aim_core_tests.exe','aim_runtime_tests.exe','aim_ballistics_tests.exe','aim_actions_tests.exe','aim_spread_tests.exe','aim_smoke_tests.exe','aim_chance_tests.exe','aim_hitboxes_tests.exe','aim_performance_tests.exe','server_rules_tests.exe','server_rule_filters_tests.exe','esp_tests.exe')) {
    Copy-Item -LiteralPath (Join-Path $taskSourceDist $taskBinary) -Destination (Join-Path $taskDist $taskBinary)
}
Push-Location $taskDist
try {
    & (Join-Path $taskDist 'feature_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Feature tests failed' }
    Copy-Item -LiteralPath 'feature-tests.json' -Destination (Join-Path $taskResults 'features.json')
    $taskTransactions = & (Join-Path $taskDist 'skin_transaction_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Skin transaction tests failed' }
    $taskTransactions | Write-Output
    [pscustomobject]@{passed=$true; output=($taskTransactions -join "`n")} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskResults 'skin-transactions.json') -Encoding UTF8
    foreach ($taskFixture in @('core','runtime','ballistics','actions','spread','smoke','chance','hitboxes','performance')) {
        & (Join-Path $taskDist "aim_${taskFixture}_tests.exe")
        if ($LASTEXITCODE -ne 0) { throw "Aim $taskFixture tests failed" }
        Copy-Item -LiteralPath "aim-${taskFixture}-tests.json" -Destination (Join-Path $taskResults "aim-${taskFixture}.json")
    }
    foreach ($taskFixture in @('server_rules','server_rule_filters')) {
        $taskServerResult = & (Join-Path $taskDist "${taskFixture}_tests.exe")
        if ($LASTEXITCODE -ne 0) { throw "$taskFixture tests failed" }
        $taskServerResult | Write-Output
        [pscustomobject]@{passed=$true; checks=[int]([regex]::Match(($taskServerResult -join "`n"),'\d+').Value); output=($taskServerResult -join "`n")} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskResults "${taskFixture}.json") -Encoding UTF8
    }
    & (Join-Path $taskDist 'esp_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'ESP tests failed' }
    Copy-Item -LiteralPath 'esp-tests.json' -Destination (Join-Path $taskResults 'esp.json')
} finally { Pop-Location }

if ($UnitOnly) {
    Write-Output 'Unit fixtures passed; DX11 and loader integration tests require a desktop session.'
    return
}

function Invoke-Smoke {
    param([string]$Name, [string[]]$TestArguments)
    $taskTestProfile = Join-Path $taskDist 'hvh-menu.ini'
    if (Test-Path -LiteralPath $taskTestProfile) { Remove-Item -LiteralPath $taskTestProfile }
    if ($TestArguments -contains '--crosshair-test') {
        "[Crosshair]`r`nEnabled=1`r`nOnlyInGame=0`r`nOnlyAlive=0`r`nHideInMenu=1`r`nStyle=2`r`nLength=12000`r`n" | Set-Content -LiteralPath $taskTestProfile -Encoding ASCII
    }
    if ($TestArguments -contains '--skins-test') {
        "[Skins]`r`nEnabled=1`r`nKnifeT=515`r`nGlovesT=5030`r`n[Skin.7]`r`nEnabled=1`r`nPaint=282`r`nSeed=337`r`nWear=180000`r`nStatTrak=123`r`n" | Set-Content -LiteralPath $taskTestProfile -Encoding ASCII
    }
    if ($TestArguments -contains '--skin-apply-test') {
        "[Skins]`r`nEnabled=0`r`n[Skin.7]`r`nEnabled=0`r`nPaint=282`r`nSeed=337`r`nWear=180000`r`nStatTrak=123`r`n[Skin.61]`r`nEnabled=0`r`nPaint=705`r`n" | Set-Content -LiteralPath $taskTestProfile -Encoding ASCII
    }
    if ($TestArguments -contains '--aim-test') {
        "[Aim]`r`nEnabled=0`r`nActivation=0`r`nKey=5`r`n[Aim.Group.4]`r`nOverrideGlobal=1`r`nFov=45000`r`n[Aim.Group.6]`r`nOverrideGlobal=1`r`nFov=12000`r`n" | Set-Content -LiteralPath $taskTestProfile -Encoding ASCII
    }
    if ($TestArguments -contains '--esp-test') {
        "[ESP]`r`nEnabled=0`r`nBoxStyle=2`r`nNames=1`r`nHealthBar=1`r`nHealthValue=1`r`nWeapons=1`r`nDistance=1`r`n" | Set-Content -LiteralPath $taskTestProfile -Encoding ASCII
        if ($TestArguments -contains '--esp-style') {
            "Skeleton=1`r`nAmmo=1`r`nAmmoBar=1`r`nStatus=1`r`n" | Add-Content -LiteralPath $taskTestProfile -Encoding ASCII
        }
    }
    $taskStarted = [DateTime]::UtcNow
    $taskProcess = Start-Process -FilePath (Join-Path $taskDist 'dx11_test_host.exe') -ArgumentList (@('--smoke-test') + $TestArguments) -WindowStyle Hidden -PassThru
    try {
        if (-not $taskProcess.WaitForExit(45000)) { throw "$Name timed out" }
        $taskProcess.Refresh()
        if ($taskProcess.ExitCode -ne 0) { throw "$Name failed, exit code $($taskProcess.ExitCode). Inspect dist\smoke-test.json and dist\hvh-menu.log." }
        $taskReportPath = Join-Path $taskDist 'smoke-test.json'
        $taskReport = Get-Item -LiteralPath $taskReportPath
        if ($taskReport.LastWriteTimeUtc -lt $taskStarted) { throw "$Name did not produce a fresh result" }
        $taskData = Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
        if (-not $taskData.passed) { throw "$Name reported failure: $($taskData.failure)" }
        Copy-Item -LiteralPath $taskReportPath -Destination (Join-Path $taskResults "$Name.json")
        Write-Output "$Name PASS: $($taskData.frames) rendered frames; $($taskData.overlay_pixels) overlay pixels; $($taskData.resizes) resizes; unloaded=$($taskData.unloaded)"
    } finally {
        if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
        $taskProcess.Dispose()
    }
}

Invoke-Smoke -Name 'hardware-present' -TestArguments @()
Invoke-Smoke -Name 'warp-present' -TestArguments @('--warp')
Invoke-Smoke -Name 'hardware-present1' -TestArguments @('--present1')
Invoke-Smoke -Name 'crosshair-render' -TestArguments @('--crosshair-test')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'menu-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'Preview conversion failed' }
Invoke-Smoke -Name 'skins-render' -TestArguments @('--skins-test')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'skins-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'Skins preview conversion failed' }
Invoke-Smoke -Name 'skin-apply-save' -TestArguments @('--skin-apply-test')
Invoke-Smoke -Name 'aim-toggle-save' -TestArguments @('--aim-test')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'aim-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'Aim preview conversion failed' }
Invoke-Smoke -Name 'esp-toggle-save' -TestArguments @('--esp-test')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'esp-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'ESP preview conversion failed' }
Invoke-Smoke -Name 'esp-style-preview' -TestArguments @('--esp-test','--esp-style')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'esp-style-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'ESP style preview conversion failed' }
Invoke-Smoke -Name 'esp-details-save' -TestArguments @('--esp-test','--esp-details')
& python (Join-Path $PSScriptRoot 'preview.py') (Join-Path $taskDist 'menu-preview.ppm') (Join-Path $taskSourceDist 'esp-details-preview.png')
if ($LASTEXITCODE -ne 0) { throw 'ESP detail preview conversion failed' }

if (-not $SkipRemote) {
    $taskHost = Start-Process -FilePath (Join-Path $taskDist 'dx11_test_host.exe') -ArgumentList '--external','--background' -WindowStyle Hidden -PassThru
    try {
        Start-Sleep -Milliseconds 500
        for ($taskCycle = 1; $taskCycle -le 2; $taskCycle++) {
            & (Join-Path $taskDist 'hvh_loader.exe') --pid $taskHost.Id
            if ($LASTEXITCODE -ne 0) { throw "Remote load cycle $taskCycle failed" }
            Start-Sleep -Milliseconds 400
            & (Join-Path $taskDist 'hvh_loader.exe') --pid $taskHost.Id --unload
            if ($LASTEXITCODE -ne 0) { throw "Remote unload cycle $taskCycle failed" }
        }
        if ($taskHost.HasExited) { throw 'External host exited unexpectedly' }
        '{"passed":true,"load_unload_cycles":2}' | Set-Content -LiteralPath (Join-Path $taskResults 'remote-loader.json') -Encoding UTF8
        Write-Output 'remote-loader PASS: 2 load/unload cycles, host remains running'
    } finally {
        if (-not $taskHost.HasExited) { Stop-Process -Id $taskHost.Id }
        $taskHost.Dispose()
    }
}
