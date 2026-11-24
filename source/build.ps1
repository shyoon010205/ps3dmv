param(
  [string]$Generator = '',
  [ValidateRange(1,64)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$env:MSBUILDDISABLENODEREUSE = '1'

function Invoke-CMake([string[]]$Arguments) {
  & $cmake @Arguments
  if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE" }
}

function Build-Target([string]$Build, [string]$Target) {
  $project = @(Get-ChildItem -LiteralPath $Build -Recurse -File -Filter "$Target.vcxproj")
  if ($project.Count -eq 1) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer vswhere.exe was not found' }
    $installation = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath).Trim()
    if (-not $installation) { throw 'A Visual Studio installation with MSBuild was not found' }
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild)) { $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe' }
    & $msbuild $project[0].FullName /p:Configuration=Release /p:Platform=x64 /m:1 /nr:false /v:m
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }
    return
  }
  Invoke-CMake @('--build',$Build,'--config','Release','--target',$Target,'--parallel',[string]$Jobs)
}

function Configure-Project([string]$Source, [string]$Build, [string[]]$Options) {
  $arguments = @('--fresh','-S',$Source,'-B',$Build,'-DCMAKE_CXX_FLAGS=/utf-8','-DCMAKE_C_FLAGS=/utf-8')
  if ($Generator) {
    $arguments += @('-G',$Generator)
    if ($Generator -like 'Visual Studio*') { $arguments += @('-A','x64') }
  }
  $arguments += $Options
  Invoke-CMake $arguments
}

function Copy-ReleaseBinary([string]$Source, [string]$Name, [string]$Destination) {
  $matches = @(Get-ChildItem -LiteralPath (Join-Path $Source 'bin') -Recurse -File -Filter $Name |
    Where-Object { $_.FullName -match '[\\/]release[^\\/]*[\\/]' })
  if ($matches.Count -ne 1) { throw "Expected one Release binary named $Name under $Source" }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  Copy-Item -LiteralPath $matches[0].FullName -Destination (Join-Path $Destination $Name) -Force
}

$projects = @(
  [pscustomobject]@{ Name='hm'; Source='HM-PS3DMVP-Direct'; Targets=@('TAppEncoder','TAppDecoder'); Files=@('TAppEncoder.exe','TAppDecoder.exe'); Options=@('-DENABLE_SEARCH_OPENSSL=OFF','-DHIGH_BITDEPTH=OFF') },
  [pscustomobject]@{ Name='vvenc'; Source='VVenC-PS3DMVP'; Targets=@('vvencFFapp'); Files=@('vvencFFapp.exe'); Options=@('-DVVENC_ENABLE_X86_SIMD=ON','-DVVENC_ENABLE_WERROR=OFF','-DBUILD_SHARED_LIBS=OFF') },
  [pscustomobject]@{ Name='vvdec'; Source='VVenC-PS3DMVP/vvdec'; Targets=@('vvdecapp'); Files=@('vvdecapp.exe'); Options=@('-DVVDEC_ENABLE_X86_SIMD=OFF','-DVVDEC_ENABLE_WERROR=OFF','-DBUILD_SHARED_LIBS=OFF') }
)

foreach ($project in $projects) {
  $source = Join-Path $PSScriptRoot $project.Source
  $build = Join-Path $projectRoot ("build/" + $project.Name)
  Configure-Project $source $build $project.Options
  foreach ($target in $project.Targets) {
    Build-Target $build $target
  }
  foreach ($file in $project.Files) {
    Copy-ReleaseBinary $source $file (Join-Path $projectRoot ("build/bin/" + $project.Name))
  }
}

$toolsBuild = Join-Path $projectRoot 'build/tools'
Configure-Project (Join-Path $PSScriptRoot 'cpp') $toolsBuild @()
Build-Target $toolsBuild 'ALL_BUILD'
Write-Host 'Build complete.'
