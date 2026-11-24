param([Parameter(Mandatory)][string]$QtPrefix,
      [string]$BuildDir=(Join-Path $PSScriptRoot '../build'),
      [string]$Destination=(Join-Path $PSScriptRoot '../dist/Inkline'))
$ErrorActionPreference='Stop'
$QtPrefix=(Resolve-Path $QtPrefix).Path
$bin=Join-Path $QtPrefix 'bin'
$objdump=Join-Path $bin 'objdump.exe'
$plugins=Join-Path $QtPrefix 'share/qt6/plugins'
if (!(Test-Path $objdump)) {throw 'For this Windows deployment script use the UCRT64 Qt SDK.'}
New-Item -ItemType Directory -Force $Destination | Out-Null
$Destination=(Resolve-Path $Destination).Path
$queue=[Collections.Generic.Queue[string]]::new()
function InstallBinary($source,$relative) {
    $target=Join-Path $Destination $relative
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
    $queue.Enqueue($target)
}
InstallBinary (Join-Path $BuildDir 'Inkline.exe') 'Inkline.exe'
InstallBinary (Join-Path $plugins 'platforms/qwindows.dll') 'platforms/qwindows.dll'
$style=Join-Path $plugins 'styles/qmodernwindowsstyle.dll'
if(Test-Path $style){InstallBinary $style 'styles/qmodernwindowsstyle.dll'}
InstallBinary (Join-Path $plugins 'tls/qschannelbackend.dll') 'tls/qschannelbackend.dll'
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while($queue.Count) {
    $file=$queue.Dequeue()
    $imports=& $objdump -p $file
    if($LASTEXITCODE){throw "Cannot inspect $file"}
    foreach($line in $imports) {
        if($line -match 'DLL Name:\s*(.+)$') {
            $name=$Matches[1].Trim()
            $sdkDll=Join-Path $bin $name
            if((Test-Path $sdkDll) -and $seen.Add($name)){InstallBinary $sdkDll $name}
        }
    }
}
@('[Paths]','Plugins=.') | Set-Content -Encoding ascii (Join-Path $Destination 'qt.conf')
foreach ($pkg in 'libyaml','tomlplusplus') {
    $dest = Join-Path $Destination "licenses/$pkg"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../third_party/$pkg/LICENSE") -Destination $dest -Force
}
$licenses=Join-Path $QtPrefix 'share/licenses'
foreach($pkg in 'qt6-base','qt6-5compat','qscintilla-qt6','gcc','libgcc','libstdc++','libb2','libwinpthread','md4c','icu','double-conversion','pcre2','zstd','zlib','libpng','freetype','harfbuzz','glib2','graphite2','brotli','bzip2','libiconv','libffi','gettext-runtime','libmd4c','winpthreads'){
    $source=Join-Path $licenses $pkg
    if(Test-Path $source){
        $dest=Join-Path $Destination "licenses/$pkg"
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item "$source/*" $dest -Recurse -Force
    }
}
$icuLicense=Get-ChildItem (Join-Path $QtPrefix 'share/icu/*/LICENSE') | Select-Object -First 1
if($icuLicense){
    $dest=Join-Path $Destination 'licenses/icu'
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -LiteralPath $icuLicense.FullName -Destination $dest -Force
}
Copy-Item (Join-Path $PSScriptRoot '../README.md') $Destination -Force
Write-Output $Destination
