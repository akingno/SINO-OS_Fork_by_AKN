# AKN fork: provenance and licensing / 来源与许可

Established: 2026-11-02.

- Fork: [akingno/SINO-OS_Fork_by_AKN](https://github.com/akingno/SINO-OS_Fork_by_AKN).
- Upstream: [SkysoftStudio-Felix/SINO-OS](https://github.com/SkysoftStudio-Felix/SINO-OS).
- Initial upstream commit: `bf0cad19002adf8f8f6d8be3efce1c044b05e252` (SINO OS 0.10 / SysBASIC 0.3.0).
- Fork maintainer: akingno (AKN). This fork is independently maintained; attribution does not imply upstream endorsement.

## Project code

The upstream project code remains under MIT. The original `Copyright (c) 2026 SINO contributors` notice and the complete permission and warranty text are retained. A separate copyright line identifies AKN's contributed modifications; it does not claim authorship of upstream or third-party material. Fork project-code modifications are also made available under MIT.

The full project license is kept in [LICENSE](../../LICENSE), [Source Code/LICENSE](<../../Source Code/LICENSE>) and [SINO-MIT.txt](SINO-MIT.txt). The image builder includes the source-tree license as `/boot/license.txt`, so newly built boot images and ISOs carry the project notice and full terms. Binary redistributions must also retain the applicable third-party notices, rather than relying only on a repository URL.

本 fork 保留原作者版权和 MIT 完整条款，新增署名仅对应 AKN 的修改。MIT 不要求为 fork 另造一种许可证；这里继续使用 MIT，并清楚区分上游、衍生修改和第三方资源。

## Third-party material

The project MIT license does not relicense the components below. Existing notices, attribution and source/version records are retained unchanged.

| Component | Applicable terms and retained records |
| --- | --- |
| Jarrah / Kurrajong / Mallee bitmap glyph artwork by Ben Letchford | SIL OFL 1.1; see [font notice](<Bitmap Font OFL License.txt>) and `Source Code/tools/fonts/platinum/OFL.txt`, `sources.json`. The upstream notice specifies the Reserved Font Name “Systemless”; respect OFL requirements when modifying or redistributing font software. This does not import the rest of the systemless emulator under MIT. |
| GNU Unifont 16.0.04 glyph subset | This project follows upstream SINO's choice of SIL OFL 1.1 for the font data; see [Unifont notice](<Chinese Font Unifont License.txt>), [README](Unifont-README.txt) and `Source Code/tools/fonts/unifont/`. Unifont's non-font source programs have separate terms; the font choice does not relicense those programs. |
| mozillazg/pinyin-data | MIT; retain its own copyright and full notice in [Pinyin Data MIT License.txt](<Pinyin Data MIT License.txt>) and `Source Code/tools/pinyin/LICENSE.txt`. |
| Bundled NASM 3.02 | BSD-2-Clause; retain [NASM-LICENSE.txt](NASM-LICENSE.txt) and `Source Code/tools/nasm-3.02/LICENSE`, including the binary redistribution notice. |
| QEMU, its firmware and other host tools | Installed separately for this development workflow. Their licenses continue to apply if a future distribution bundles them; this repository's MIT does not cover them. |

Boot resources already retain the font/Pinyin notices as `FONT_OFL.TXT`, `CJK_OFL.TXT` and `PINYIN.TXT`. Full component provenance is in [Code and Third-Party Sources.md](<Code and Third-Party Sources.md>) and [Source Code/SOURCES.md](<../../Source Code/SOURCES.md>).

Future copied or adapted implementation code must be reviewed under its actual license and recorded before inclusion. In particular, using early Linux as a teaching/design reference does not make Linux implementation code MIT-licensed. No Linux implementation was imported by the changes establishing this fork.

## Scope of the first fork changes

The first fork changes establish a source-to-QEMU development workflow: optional build identification, separate run artifacts and a development disk, source/ISO identity checks, and a Ring 3 smoke check. They also document fork provenance and include the MIT notice in boot resources. These changes do not implement the planned editor, host bridge or server connection.

Existing upstream release assets, screenshots, SHA manifests and historical verification summaries remain historical upstream material. They are not certification of the modified fork. Development output, private VM data, extracted release copies and personal learning notes are excluded by `.gitignore` and remain local.

For the MIT text, see the [Open Source Initiative](https://opensource.org/license/mit); for font terms, consult the retained license texts and the [SIL OFL](https://openfontlicense.org/open-font-license-official-text/). This document records the repository's licensing choices and distribution practice; individual retained license texts remain authoritative.
