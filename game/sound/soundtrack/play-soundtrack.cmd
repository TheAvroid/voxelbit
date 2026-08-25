@echo off
rem Shuffles every .mp3 sitting next to this file into soundtrack.m3u, then opens it.
rem Double-click this instead of the .m3u and you get a fresh order every time.
setlocal
set "D=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='%D%'; $o=Join-Path $d 'soundtrack.m3u'; $f=Get-ChildItem -LiteralPath $d -Filter *.mp3 | Sort-Object {Get-Random} | ForEach-Object {$_.Name}; Set-Content -LiteralPath $o -Value (@('#EXTM3U','#PLAYLIST:Voxelbit Soundtrack') + $f) -Encoding ascii; if ($env:VB_NOPLAY -ne '1') { Start-Process -FilePath $o }"
