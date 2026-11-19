$ErrorActionPreference = 'Stop'
$base = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -AssemblyName System.Drawing
$runtime = Split-Path ([Drawing.Bitmap].Assembly.Location)
$refs = @(
    [object].Assembly.Location,
    [Drawing.Bitmap].Assembly.Location,
    [Drawing.Color].Assembly.Location,
    (Join-Path $runtime 'System.Private.Windows.GdiPlus.dll'),
    (Join-Path $runtime 'System.Private.Windows.Core.dll'),
    (Join-Path $runtime 'System.Runtime.dll')
) | Select-Object -Unique
Add-Type -Path (Join-Path $base 'AugmentAtlas.cs') -ReferencedAssemblies $refs
$source = [IO.File]::ReadAllText((Join-Path $base 'atlas.py'))
$start = $source.IndexOf('CJK =')
$end = $source.IndexOf('CHARS =')
if ($start -lt 0 -or $end -le $start) { throw 'Could not locate CJK set' }
$set = New-Object 'System.Collections.Generic.HashSet[char]'
foreach ($m in [regex]::Matches($source.Substring($start, $end - $start), '[\u4e00-\u9fff]')) {
    [void]$set.Add($m.Value[0])
}
[AugmentAtlas]::Run((Join-Path $base 'atlas.bin'),
                    (Join-Path $base 'atlas-augmented.bin'),
                    (-join [char[]]$set))
