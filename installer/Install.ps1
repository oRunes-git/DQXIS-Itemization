param(
    [string]$GamePath,
    [string]$InstallPath=(Join-Path $env:LOCALAPPDATA 'DQXI-Itemization\Runtime'),
    [string]$ExistingRuntimePath,
    [switch]$NoShortcut,
    [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2

function Get-PackageHash([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}
    finally{$sha.Dispose();$stream.Dispose()}
}

function Get-SafePath([string]$Root,[string]$Relative) {
    if([IO.Path]::IsPathRooted($Relative)){throw 'Invalid package path.'}
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'
    $path=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if(-not $path.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){throw 'Package path leaves its folder.'}
    return $path
}
function Get-GameRoot([string]$Selected) {
    $path=[IO.Path]::GetFullPath($Selected)
    if(Test-Path -LiteralPath $path -PathType Leaf){
        if([IO.Path]::GetFileName($path) -ine 'DRAGON QUEST XI S.exe'){throw 'Choose DRAGON QUEST XI S.exe.'}
        $path=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $path)))
    }
    $exe=Join-Path $path 'Game\Binaries\Win64\DRAGON QUEST XI S.exe'
    $paks=Join-Path $path 'Game\Content\Paks'
    if(-not(Test-Path -LiteralPath $exe -PathType Leaf) -or -not(Test-Path -LiteralPath $paks -PathType Container)){throw 'The selected folder is not Dragon Quest XI S.'}
    return $path
}
function Find-Game {
    $libraries=New-Object 'System.Collections.Generic.List[string]'
    foreach($key in @('HKCU:\Software\Valve\Steam','HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')){
        $entry=Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if($entry){foreach($property in @('SteamPath','InstallPath')){
            $value=$entry.PSObject.Properties[$property]
            if($value -and $value.Value -and -not $libraries.Contains([string]$value.Value)){$libraries.Add([string]$value.Value)}
        }}
    }
    foreach($steam in @($libraries.ToArray())){
        $vdf=Join-Path $steam 'steamapps\libraryfolders.vdf'
        if(Test-Path -LiteralPath $vdf){
            $content=[IO.File]::ReadAllText($vdf)
            foreach($match in [regex]::Matches($content,'"path"\s*"([^"]+)"')){
                $library=$match.Groups[1].Value.Replace('\\','\')
                if(-not $libraries.Contains($library)){$libraries.Add($library)}
            }
        }
    }
    foreach($library in $libraries){
        $acf=Join-Path $library 'steamapps\appmanifest_1295510.acf'
        if(-not(Test-Path -LiteralPath $acf)){continue}
        $match=[regex]::Match([IO.File]::ReadAllText($acf),'"installdir"\s*"([^"]+)"')
        if($match.Success){
            $candidate=Join-Path (Join-Path $library 'steamapps\common') $match.Groups[1].Value
            try{return Get-GameRoot $candidate}catch{continue}
        }
    }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog=New-Object Windows.Forms.OpenFileDialog
    $dialog.Title='Choose DRAGON QUEST XI S.exe in Game\Binaries\Win64'
    $dialog.Filter='Dragon Quest XI S|DRAGON QUEST XI S.exe'
    if($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK){throw 'Installation cancelled.'}
    return Get-GameRoot $dialog.FileName
}
function Find-PreviousHistory {
    if($ExistingRuntimePath){
        $candidate=Join-Path ([IO.Path]::GetFullPath($ExistingRuntimePath)) 'state\item-history.json'
        if(-not(Test-Path -LiteralPath $candidate -PathType Leaf)){throw 'The previous runtime has no item history.'}
        return $candidate
    }
    $desktop=[Environment]::GetFolderPath('Desktop')
    $shell=New-Object -ComObject WScript.Shell
    foreach($file in @(Get-ChildItem -LiteralPath $desktop -Filter '*Itemization*.lnk' -File -ErrorAction SilentlyContinue)){
        $shortcut=$shell.CreateShortcut($file.FullName)
        $candidates=New-Object 'System.Collections.Generic.List[string]'
        if($shortcut.WorkingDirectory){$candidates.Add($shortcut.WorkingDirectory)}
        if([IO.Path]::GetFileName($shortcut.TargetPath) -ieq 'Launch-itemization.cmd'){$candidates.Add((Split-Path -Parent $shortcut.TargetPath))}
        $match=[regex]::Match($shortcut.Arguments,'(?i)"([^"]+\\Launch-itemization\.(?:cmd|ps1))"')
        if($match.Success){$candidates.Add((Split-Path -Parent $match.Groups[1].Value))}
        foreach($folder in $candidates){
            $history=Join-Path $folder 'state\item-history.json'
            if(Test-Path -LiteralPath $history -PathType Leaf){return [IO.Path]::GetFullPath($history)}
        }
    }
    return $null
}
function Assert-History([string]$Path) {
    $history=[IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if($history.schema_version -ne 1 -or $history.revisions -isnot [Array]){throw 'Existing item history is invalid. Nothing has been replaced.'}
}

$manifestPath=Join-Path $PSScriptRoot 'package-manifest.json'
$manifest=[IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
if($manifest.schema_version -ne 1 -or $manifest.version -ne '1.0'){throw 'Unsupported package manifest.'}
Write-Host 'Checking the mod package...'
foreach($entry in $manifest.files.PSObject.Properties){
    $source=Get-SafePath $PSScriptRoot $entry.Name
    if(-not(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-PackageHash $source) -ine $entry.Value){throw "Package file is missing or changed: $($entry.Name). Extract a fresh copy."}
}
$gameRoot=if($GamePath){Get-GameRoot $GamePath}else{Find-Game}
$gameExe=Join-Path $gameRoot 'Game\Binaries\Win64\DRAGON QUEST XI S.exe'
if((Get-PackageHash $gameExe) -ine $manifest.supported_game_exe_sha256){throw 'This executable differs from the supported Steam XI S build 6291077. Nothing was installed.'}
$pakRoot=Join-Path $gameRoot 'Game\Content\Paks'
$installRoot=[IO.Path]::GetFullPath($InstallPath)
if($installRoot.TrimEnd('\') -ieq [IO.Path]::GetPathRoot($installRoot).TrimEnd('\')){throw 'Choose a dedicated mod folder.'}
$historyPath=Get-SafePath $installRoot 'state\item-history.json'
foreach($relative in $manifest.runtime_files){
    $target=Get-SafePath $installRoot $relative
    if((Test-Path -LiteralPath $target) -and -not(Test-Path -LiteralPath $target -PathType Leaf)){throw 'A mod file destination is occupied by a folder.'}
}
foreach($name in $manifest.pak_files){
    $target=Get-SafePath $pakRoot $name
    if((Test-Path -LiteralPath $target) -and -not(Test-Path -LiteralPath $target -PathType Leaf)){throw 'A mod archive destination is occupied by a folder.'}
}
$importHistory=$null
if(Test-Path -LiteralPath $historyPath -PathType Leaf){Assert-History $historyPath}else{
    $importHistory=Find-PreviousHistory
    if($importHistory){Assert-History $importHistory}
}
$obsolete=New-Object 'System.Collections.Generic.List[string]'
foreach($entry in $manifest.obsolete_archives.PSObject.Properties){
    $path=Get-SafePath $pakRoot $entry.Name
    if(Test-Path -LiteralPath $path -PathType Leaf){
        if((Get-PackageHash $path) -ine $entry.Value){throw "A conflicting old mod archive differs from the known version: $($entry.Name). Nothing was installed."}
        $obsolete.Add($path)
    }
}
if($ValidateOnly){
    [pscustomobject]@{valid=$true;version=$manifest.version;game_root=$gameRoot;install_root=$installRoot;existing_history_preserved=(Test-Path -LiteralPath $historyPath);history_import=$importHistory;package_files=@($manifest.files.PSObject.Properties).Count}
    return
}
if(@(Get-Process -Name 'DRAGON QUEST XI S' -ErrorAction SilentlyContinue).Count){throw 'Close Dragon Quest XI S before installing.'}

$backupRoot=Get-SafePath $installRoot ('backups\'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$snapshots=New-Object 'System.Collections.Generic.List[object]'
function Save-Previous([string]$Target){
    if(@($snapshots | Where-Object {$_.target -ieq $Target}).Count){return}
    $existed=Test-Path -LiteralPath $Target -PathType Leaf
    $backup=Join-Path $backupRoot ($snapshots.Count.ToString('D4')+'.backup')
    if($existed){Copy-Item -LiteralPath $Target -Destination $backup}
    $snapshots.Add([pscustomobject]@{target=$Target;existed=$existed;backup=$backup})
}
$shortcutPath=Join-Path ([Environment]::GetFolderPath('Desktop')) 'DQXI S - Itemization.lnk'
$receiptPath=Get-SafePath $installRoot 'installation.json'
try {
    Write-Host 'Installing Itemization...'
    foreach($relative in $manifest.runtime_files){
        $source=Get-SafePath $PSScriptRoot ('Payload\Runtime\'+$relative)
        $target=Get-SafePath $installRoot $relative
        Save-Previous $target
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    Save-Previous $historyPath
    if(-not(Test-Path -LiteralPath $historyPath)){
        New-Item -ItemType Directory -Path (Split-Path -Parent $historyPath) -Force | Out-Null
        if($importHistory){
            Copy-Item -LiteralPath $importHistory -Destination (Join-Path $backupRoot 'imported-item-history.json')
            Copy-Item -LiteralPath $importHistory -Destination $historyPath
        }else{Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Payload\Fresh-item-history.json') -Destination $historyPath}
    }
    $historyHash=(Get-PackageHash $historyPath)
    foreach($name in $manifest.pak_files){
        $target=Get-SafePath $pakRoot $name;Save-Previous $target
        Copy-Item -LiteralPath (Get-SafePath $PSScriptRoot ('Payload\Paks\'+$name)) -Destination $target -Force
    }
    foreach($path in $obsolete){Save-Previous $path;Remove-Item -LiteralPath $path}
    foreach($relative in $manifest.runtime_files){
        $expected=$manifest.files.PSObject.Properties['Payload/Runtime/'+$relative.Replace('\','/')].Value
        if((Get-PackageHash (Get-SafePath $installRoot $relative)) -ine $expected){throw 'Installed mod verification failed.'}
    }
    foreach($name in $manifest.pak_files){
        $expected=$manifest.files.PSObject.Properties['Payload/Paks/'+$name].Value
        if((Get-PackageHash (Get-SafePath $pakRoot $name)) -ine $expected){throw 'Installed artwork verification failed.'}
    }
    if(-not $NoShortcut){
        Save-Previous $shortcutPath
        $shell=New-Object -ComObject WScript.Shell
        $shortcut=$shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath=Join-Path $env:WINDIR 'System32\cmd.exe'
        $shortcut.Arguments='/d /c ""'+(Get-SafePath $installRoot 'Launch-itemization.cmd')+'""'
        $shortcut.WorkingDirectory=$installRoot;$shortcut.IconLocation=$gameExe+',0'
        $shortcut.Description='Dragon Quest XI S - Itemization 1.0';$shortcut.Save()
    }
    if((Get-PackageHash $historyPath) -ne $historyHash){throw 'Item history changed unexpectedly.'}
    Save-Previous $receiptPath
    $receipt=[ordered]@{installed=$true;version=$manifest.version;game_root=$gameRoot;install_root=$installRoot;backup=$backupRoot;history_imported=[bool]$importHistory;history_sha256=$historyHash;history_preserved=$true;game_saves_modified=$false;game_started=$false;shortcut_created=(-not $NoShortcut);files=@($snapshots.ToArray())}
    [IO.File]::WriteAllText($receiptPath,($receipt | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $backupRoot 'restore-map.json'),($snapshots.ToArray() | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    Write-Host 'Installed. Use the DQXI S - Itemization shortcut and wait for Itemization is ready before loading a log.'
    Write-Host ('Your saves were not changed. Mod backup: '+$backupRoot)
} catch {
    $failure=$_
    $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
    for($index=$snapshots.Count-1;$index -ge 0;--$index){
        $entry=$snapshots[$index]
        try{
            if($entry.existed){Copy-Item -LiteralPath $entry.backup -Destination $entry.target -Force}
            elseif(Test-Path -LiteralPath $entry.target -PathType Leaf){Remove-Item -LiteralPath $entry.target}
        }catch{$restoreErrors.Add($_.Exception.Message)}
    }
    if($restoreErrors.Count){throw ('Installation failed; some files could not be restored. Keep the backup at '+$backupRoot+'. '+($restoreErrors -join '; '))}
    throw $failure
}
