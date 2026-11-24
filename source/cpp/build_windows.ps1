param([ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$RunTests, [switch]$Fresh)
$buildDirectory = Join-Path $PSScriptRoot 'build'
$cmakeExecutable = (Get-Command cmake -ErrorAction Stop).Source
$ctestExecutable = Join-Path (Split-Path $cmakeExecutable) 'ctest.exe'
function Invoke-BuildTool([string]$Executable, [string[]]$ToolArguments) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = $PSScriptRoot
    # Normalize inherited environment names for MSBuild's case-insensitive lookup.
    $environmentCopy = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $environmentCopy[[string]$entry.Key] = [string]$entry.Value
    }
    $startInfo.Environment.Clear()
    foreach ($entry in $environmentCopy.GetEnumerator()) { $startInfo.Environment[$entry.Key] = $entry.Value }
    # Dependencies are vendored; avoid importing unrelated per-user vcpkg settings.
    $startInfo.Environment['VCLibPackagePath'] = Join-Path $buildDirectory 'no_external_vcpkg'
    foreach ($argument in $ToolArguments) { $startInfo.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "$Executable failed with exit code $($process.ExitCode)" }
}
$configureArguments = @('-S',$PSScriptRoot,'-B',$buildDirectory,'-G','Visual Studio 18 2026','-A','x64')
if ($Fresh) { $configureArguments += '--fresh' }
Invoke-BuildTool $cmakeExecutable $configureArguments
Invoke-BuildTool $cmakeExecutable @('--build',$buildDirectory,'--config',$Configuration,'--parallel','4')
if ($RunTests) { Invoke-BuildTool $ctestExecutable @('--test-dir',$buildDirectory,'-C',$Configuration,'--output-on-failure') }
