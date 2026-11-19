# Third-party notices

Tinker Editor redistributes the following components. Their licences remain with
their respective copyright holders.

## cmark-gfm 0.29.0.gfm.13

Markdown parsing and HTML rendering use
[cmark-gfm](https://github.com/github/cmark-gfm), copyright (c) 2014 John
MacFarlane and contributors. Its BSD 2-Clause terms and the notices for derived
components are reproduced at `third_party/cmark-gfm/COPYING`. The vendored
`src/config.h` (generated per compiler upstream) is modified to enable its
GCC-only features only under GCC/Clang, so the same copy also builds with MSVC.

## Mermaid 11.16.0

`mermaid.min.js` is from [Mermaid](https://github.com/mermaid-js/mermaid),
copyright (c) 2014–2022 Knut Sveidqvist and contributors, licensed under the
MIT License. A copy is at `third_party/mermaid/LICENSE`.

## GNU GDB 17.1

C/C++ debugging launches a separate copy of
[GNU GDB](https://sourceware.org/gdb/) built by w64devkit 2.7.0. GDB is not
linked into `tinkeredit.exe`; it remains a standalone program distributed under
the GNU General Public License, version 3. The licence is reproduced at
`third_party/gdb/COPYING.txt`.

The complete corresponding source inputs, including GDB 17.1, its statically
linked Expat 2.7.5, GNU libiconv 1.19 and PDCurses 3.9 sources, plus the exact
w64devkit Dockerfile and GDB patches used for this binary, are available in the
[Tinker Editor source repository](https://github.com/LesFarrell/Tinker Editor/tree/main/third_party/gdb/source).
They are not duplicated in the smaller binary package. Provenance and checksums are recorded in
`third_party/gdb/SOURCE.md` and `third_party/gdb/SHA256SUMS.txt`. Applicable
MinGW-w64 runtime notices are reproduced at
`third_party/gdb/COPYING.MinGW-w64-runtime.txt`.

## Highlight.js 11.11.1

Fenced-code syntax highlighting uses
[Highlight.js](https://github.com/highlightjs/highlight.js), copyright (c) 2006
Ivan Sagalaev and contributors, licensed under the BSD 3-Clause License. A copy
is at `third_party/highlightjs/LICENSE`.

## KaTeX 0.17.0

Mathematical expressions are rendered by
[KaTeX](https://github.com/KaTeX/KaTeX), copyright (c) 2013–2020 Khan
Academy and other contributors, licensed under the MIT License. A copy is at
`third_party/katex/LICENSE`.

## github-markdown-css

`github-markdown.css` is derived from
[github-markdown-css](https://github.com/sindresorhus/github-markdown-css),
copyright (c) Sindre Sorhus, licensed under the MIT License. A copy is at
`third_party/github-markdown-css/LICENSE`.

## Microsoft WebView2 SDK 1.0.4078.44

The headers, import library, and `WebView2Loader.dll` come from the
[Microsoft.Web.WebView2 1.0.4078.44 package](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4078.44).
Copyright (c) Microsoft Corporation. Redistribution is permitted under the
three-clause BSD-style licence published with that package; a copy is at
`third_party/webview2/LICENSE.txt`.

## Scintilla 5.6.4

Scintilla headers and `Scintilla.dll` are copyright 1998–2021 by Neil Hodgson
and contributors and are distributed under Scintilla's permissive licence.
A copy is at `third_party/scintilla/License.txt`.

## Lexilla 5.5.1

Source-editor syntax highlighting uses lexers from
[Lexilla](https://github.com/ScintillaOrg/lexilla), copyright 1998–2021 by
Neil Hodgson and contributors, distributed under the same permissive licence
as Scintilla. It's built from a trimmed copy of the upstream source under
`third_party/lexilla` and linked directly into `tinkeredit.exe`, carrying the
Markdown, JSON, XML/HTML, YAML, C/C++, Python, Bash, PowerShell, Lua, CSS,
SQL, properties (INI), TOML, batch and makefile lexers with their required
support code; a copy of the licence is at
`third_party/lexilla/License.txt`.

## SCOWL 2026.02.25

The anagram plugin's British English word list
(`plugins/examples/anagrams/words_en_gb.lua`) is generated from
[SCOWL](https://github.com/en-wl/wordlist) (Spell Checker Oriented Word
Lists), copyright 2000–2026 by Kevin Atkinson, which is derived from many
sources, most of them in the public domain. It is distributed under SCOWL's
permissive licence, reproduced with the notices for its sources at
`third_party/scowl/COPYING` and beside the word list at
`plugins/examples/anagrams/COPYING`. How the list was made, with checksums, is recorded
in `third_party/scowl/VERSION.txt`.

## Lua 5.4.9

Plugins run on [Lua](https://www.lua.org/), copyright (C) 1994–2026 Lua.org,
PUC-Rio, licensed under the MIT License. It's built from the unmodified
release sources under `third_party/lua` and linked directly into
`tinkeredit.exe`; a copy of the licence is at `third_party/lua/LICENSE`.
