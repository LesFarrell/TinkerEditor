[CmdletBinding()]
param(
    [string]$StartAt
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$testRoot = Join-Path $repo "build\tests"
$objectRoot = Join-Path $repo "build\test-obj-msvc"
New-Item -ItemType Directory -Force -Path $testRoot, $objectRoot | Out-Null
Copy-Item -LiteralPath "third_party\scintilla\bin\Scintilla.dll" -Destination "build\Scintilla.dll" -Force

function Get-RspSources([string]$Path) {
    Get-Content -LiteralPath $Path |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '\.(c|cc|cpp|cxx)$' } |
        ForEach-Object { Join-Path $repo $_ }
}

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repo $Path
}

function Invoke-Tool([string]$File, [string[]]$Arguments, [string]$Failure) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Failure (exit code $LASTEXITCODE)."
    }
}

$commonOptions = @(
    "/nologo", "/std:c11", "/O1", "/GL", "/Gy", "/Gw", "/MT",
    "/DWIN32", "/DNDEBUG", "/D_CRT_SECURE_NO_WARNINGS", "/utf-8", "/W4", "/WX",
    "/wd4310", "/wd4702",
    "/I$repo", "/I$(Join-Path $repo 'third_party\lua\src')"
)
$luaSources = @(Get-RspSources "lua-sources.rsp")
$pluginSources = @(Get-RspSources "plugin-sources.rsp")

$luaObjectDir = Join-Path $objectRoot "lua"
New-Item -ItemType Directory -Force -Path $luaObjectDir | Out-Null
$luaCompile = $commonOptions + @(
    "/c", "/wd4244", "/wd4310", "/wd4324", "/wd4701", "/Fo$luaObjectDir\"
) + $luaSources
Invoke-Tool "cl.exe" $luaCompile "MSVC failed compiling Lua for the tests"
$luaObjects = foreach ($source in $luaSources) {
    Join-Path $luaObjectDir (([IO.Path]::GetFileNameWithoutExtension($source)) + ".obj")
}

$cmarkObjectDir = Join-Path $objectRoot "cmark-gfm"
New-Item -ItemType Directory -Force -Path $cmarkObjectDir | Out-Null
$cmarkSources = @(Get-RspSources "cmark-gfm-sources.rsp")
$cmarkCompile = $commonOptions + @(
    "/c", "/wd4100",
    "/I$(Join-Path $repo 'third_party\cmark-gfm\src')",
    "/I$(Join-Path $repo 'third_party\cmark-gfm\extensions')",
    "/DCMARK_GFM_STATIC_DEFINE", "/DCMARK_GFM_EXTENSIONS_STATIC_DEFINE",
    "/Fo$cmarkObjectDir\"
) + $cmarkSources
Invoke-Tool "cl.exe" $cmarkCompile "MSVC failed compiling cmark-gfm for the tests"
$cmarkObjects = foreach ($source in $cmarkSources) {
    Join-Path $cmarkObjectDir (([IO.Path]::GetFileNameWithoutExtension($source)) + ".obj")
}

$tests = @(
    @{ Name="test_parser"; Sources=@("src\markdown.c"); Objects=$cmarkObjects; Libraries=@(); Cmark=$true },
    @{ Name="test_markdown_export"; Sources=@("src\markdown_export.c"); Objects=$cmarkObjects; Libraries=@(); Cmark=$true },
    @{ Name="test_editor_commands"; Sources=@("src\editor_commands.c"); Libraries=@("user32.lib") },
    @{ Name="test_text_search"; Sources=@("src\text_search.c"); Libraries=@("user32.lib") },
    @{ Name="test_language"; Sources=@("src\editor_language.c", "src\theme.c"); Libraries=@("user32.lib", "shlwapi.lib", "gdi32.lib", "uxtheme.lib") },
    @{ Name="test_outline"; Sources=@("src\outline_minimap.c", "src\theme.c", "src\text_codec.c"); Libraries=@("user32.lib", "comctl32.lib", "gdi32.lib", "msimg32.lib") },
    @{ Name="test_theme"; Sources=@("src\theme.c") + $pluginSources; Objects=$luaObjects; Libraries=@("user32.lib", "gdi32.lib", "uxtheme.lib") },
    @{ Name="test_core"; Sources=@("src\text_codec.c", "src\workspace_text.c", "src\markdown_links.c", "src\file_io.c", "src\path_utils.c", "src\plugin_settings.c"); Libraries=@() },
    @{ Name="test_output"; Sources=@("src\output_text.c", "src\output_process.c"); Libraries=@("user32.lib") },
    @{ Name="test_git_status"; Sources=@("src\git_status.c"); Libraries=@() },
    @{ Name="test_git_workflow"; Sources=@(); Libraries=@() },
    @{ Name="test_terminal_grid"; Sources=@("src\terminal_grid.c"); Libraries=@() },
    @{ Name="test_terminal_input"; Sources=@("src\terminal_input.c"); Libraries=@("user32.lib") },
    @{ Name="test_terminal_process"; Sources=@("src\terminal_process.c"); Libraries=@("user32.lib") },
    @{ Name="test_terminal_view"; Sources=@("src\terminal_view.c", "src\terminal_grid.c", "src\terminal_input.c", "src\terminal_process.c"); Libraries=@("user32.lib", "comctl32.lib", "gdi32.lib") },
    @{ Name="test_recovery"; Sources=@("src\recovery.c", "src\file_io.c", "src\text_codec.c", "src\path_utils.c"); Libraries=@("ole32.lib") },
    @{ Name="test_plugin_host"; Sources=$pluginSources; Objects=$luaObjects; Libraries=@("user32.lib") },
    @{ Name="test_assemblers"; Sources=@(); Objects=$luaObjects; Libraries=@() },
    @{ Name="test_lua_format"; Sources=@(); Objects=$luaObjects; Libraries=@() },
    @{ Name="test_menus"; Sources=@("src\menu_commands.c", "src\app_accelerators.c") + $pluginSources; Objects=$luaObjects; Libraries=@("user32.lib") },
    @{ Name="test_formatting"; Sources=@("src\menu_commands.c") + $pluginSources; Objects=$luaObjects; Libraries=@("user32.lib") }
)

$started = [string]::IsNullOrEmpty($StartAt)
foreach ($test in $tests) {
    $name = $test.Name
    if (-not $started) {
        if ($name -ne $StartAt) { continue }
        $started = $true
    }
    $testObjectDir = Join-Path $objectRoot $name
    New-Item -ItemType Directory -Force -Path $testObjectDir | Out-Null
    $executable = Join-Path $testRoot "$name.exe"
    $sources = @((Join-Path $repo "tests\$name.c")) + @($test.Sources | ForEach-Object { Resolve-RepoPath $_ })
    $options = $commonOptions + @("/Fe$executable", "/Fo$testObjectDir\")
    if ($test.Cmark) {
        $options += @(
            "/I$(Join-Path $repo 'third_party\cmark-gfm\src')",
            "/I$(Join-Path $repo 'third_party\cmark-gfm\extensions')",
            "/DCMARK_GFM_STATIC_DEFINE", "/DCMARK_GFM_EXTENSIONS_STATIC_DEFINE"
        )
    }
    $arguments = $options + $sources + @($test.Objects) + @($test.Libraries) + @("/link", "/LTCG")
    Invoke-Tool "cl.exe" $arguments "MSVC failed building $name"
    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw "$name failed (exit code $LASTEXITCODE)."
    }
}
if (-not $started) { throw "Unknown test name passed to -StartAt: $StartAt" }

$node = Get-Command node.exe -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Write-Host "Node.js not found; preview bridge tests skipped."
} else {
    Invoke-Tool $node.Source @("tests\test_preview.js") "Preview bridge tests failed"
}

Write-Host "All MSVC tests passed."
