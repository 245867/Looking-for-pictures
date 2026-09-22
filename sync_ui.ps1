# Regenerates the inlined CSS/JS data URIs inside index.html from style.css and app.js.
# JadeView cannot distribute same-page static assets reliably, so the page embeds them.
$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$web = Join-Path $dir "runtime\jade_ui\web"
$htmlPath = Join-Path $web "index.html"
$cssPath = Join-Path $web "style.css"
$jsPath = Join-Path $web "app.js"

$html = [System.IO.File]::ReadAllText($htmlPath, [System.Text.Encoding]::UTF8)
$css = [System.IO.File]::ReadAllText($cssPath, [System.Text.Encoding]::UTF8)
$js = [System.IO.File]::ReadAllText($jsPath, [System.Text.Encoding]::UTF8)

$cssB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($css))
$jsB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($js))

$html = [regex]::Replace($html, 'href="data:text/css;base64,[^"]+"', 'href="data:text/css;base64,' + $cssB64 + '"')
$html = [regex]::Replace($html, 'src="data:text/javascript;base64,[^"]+"', 'src="data:text/javascript;base64,' + $jsB64 + '"')

[System.IO.File]::WriteAllText($htmlPath, $html, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "index.html assets synced: css=$($css.Length) chars, js=$($js.Length) chars"
