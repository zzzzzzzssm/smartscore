# DOCX 模板执行契约

## Reference

- Retained reference: `D:\qianrushi\智能乐谱辅助系统_本地视觉识别与演奏抓拍模块设计文档.docx`
- Design authority: `D:\qianrushi\物联网模版.docx`
- Retained reference SHA-256: `1738FDB9045D125C5E86C54D653D7AB100A37957D8E1675A053C9886E1D3685C`
- Retained reference page count: 8 pages (Microsoft Word pagination)
- Retained reference section count: 1
- Design-authority template page count: 11 pages
- Design-authority template section count: 9
- Structural evidence: `D:\qianrushi\smartscore_s3_audio\.docx_work\extract-vision\structure.json`
- Package inventory: `D:\qianrushi\smartscore_s3_audio\.docx_work\extract-vision\package_inventory.json`
- Template page render: `D:\qianrushi\smartscore_s3_audio\.docx_work\template-pages`
- Rendering note: LibreOffice is unavailable. The 11-page design-authority template was exported by Microsoft Word and every PNG page was inspected. The retained submodule reference was structurally inspected; final output must be exported and inspected with Microsoft Word.

## Page system

- Paper: A4 portrait, 11906 x 16838 twips.
- Margins: left/right 1418 twips; top 2211 twips; bottom 2155 twips.
- Header distance: 851 twips; footer distance: 992 twips.
- One section; the first-page header/footer part duplicates the default header/footer so Word renders identical page furniture on every page; no odd/even variation.
- Header text: `智能乐谱辅助系统技术报告`, centered, with the source header rule preserved.
- Footer: centered `第 {PAGE} 页`, using the retained PAGE field.
- Body chapter `4.X` follows the result table without a forced page break so the insertable submodule does not create a nearly blank page; headings remain in the same section.

## Typography

- Normal: Microsoft YaHei for East Asian text, Calibri for Latin text, 10.5 pt, justified, 1.15 line spacing, 420-twip first-line indent, 5 pt after.
- Heading 1: Microsoft YaHei/Calibri, 16 pt, bold, left aligned, 16 pt before, 8 pt after, keep with next.
- Heading 2: Microsoft YaHei/Calibri, 13 pt, bold, left aligned, 12 pt before, 6 pt after, keep with next.
- Heading 3: Microsoft YaHei/Calibri, 11 pt, bold, left aligned, 8 pt before, 4 pt after, keep with next.
- Caption: 9 pt, bold, centered, single-spaced, 4 pt before and after.
- Table title: 10.5 pt, bold, centered, 8 pt before and 4 pt after.
- Header/footer: preserve retained source definitions and fields.
- Inline English identifiers and numeric units inherit the paragraph role; no monospace body treatment.

## Lists and tables

- Use ordinary prose for explanations; no fake bullet paragraphs.
- Use tables only for repeated comparable data: recognition-result definitions, hardware connections and task/resource allocation.
- Table width equals the usable body width: 9070 twips. All `tblW`, `tblGrid`, and cell widths must agree.
- Table indent: 0 twips so the outer border aligns with body text.
- Header rows: dark blue-gray fill, white bold text, centered vertically, repeat on page breaks.
- Body rows: vertically centered; narrative columns left aligned; pins, sizes, rates and short status values centered.
- Cell margins: 120 twips left/right and 80 twips top/bottom. No fixed row heights.
- Figures are centered and use in-line anchors. Captions remain with the figure.

## Components

- No cover, abstract, table of contents, bibliography or standalone conclusion.
- Opening component is the `3.X` Heading 1 paragraph; no extra title block.
- One function/data-flow diagram appears after 3.X.2 and uses six horizontally connected blocks across two rows if needed.
- Table 3.X-1 defines the five internal recognition result classes.
- Table 4.X-1 gives only audio-related hardware/I2C/I2S connections; it excludes UART and external-controller wiring.
- Table 4.X-2 summarizes `AudioCaptureTask` and `MusicDspTask`; it excludes the integration/communication task.
- A short concluding paragraph at the end of 4.X.5 states current capability boundaries without creating a separate summary heading.

## Content flow and capacity

1. `3.X 本地音频采集与音乐识别功能设计` — about 3 pages.
2. `3.X.1 模块定位与主要功能` — 2 concise paragraphs.
3. `3.X.2 功能组成与数据流程` — 2 paragraphs plus Figure 3.X-1.
4. `3.X.3 单音、双音及和弦识别逻辑` — 3 concise paragraphs.
5. `3.X.4 稳定判定与异常处理` — 2 paragraphs plus Table 3.X-1.
6. `4.X 本地音频采集与音乐识别模块实现` — follows 3.X without a forced page break, about 4 pages.
7. `4.X.1 硬件接口与音频采集` — 2 paragraphs plus Table 4.X-1.
8. `4.X.2 双麦预处理与通道处理` — 3 paragraphs.
9. `4.X.3 YIN、FFT及分类算法实现` — 4 paragraphs.
10. `4.X.4 FreeRTOS任务与资源组织` — 2 paragraphs plus Table 4.X-2.
11. `4.X.5 构建验证、实机测试方法与能力边界` — 3 paragraphs and compact numbered acceptance sequence.

Target length: 6–8 pages. Shorten prose or move a heading to the preceding page before reducing font size.

## Slot map

- `word/document.xml` body: replace entirely with the approved 3.X/4.X content; the final `sectPr` remains source-derived.
- `word/header1.xml`: preserve header structure and rule; preserve text.
- `word/footer1.xml`: preserve the page-number field and alignment.
- `word/styles.xml`: preserve source styles; only add a named Table Title style if missing.
- `word/numbering.xml`: preserve. Numbered acceptance steps may use a real existing numbering definition or a newly defined one.
- `word/theme/theme1.xml` and `word/fontTable.xml`: preserve.
- Existing body drawings and `word/media/*`: removable because the entire visual-module body is an editable slot.
- New `word/media/music_recognition_flow.png` and its relationship: permitted.
- Core properties: editable only to set the output title and clear personal author fields.

## Text coverage

- Replace all body paragraphs and all table cells from the retained reference.
- Verify the output body contains no visual-module terms such as `OV3660`, `Hand Detect`, `手势`, `microSD`, `point_left` or `point_right`.
- Verify the output body contains no integration/communication details such as `UART`, `NDJSON`, `poll`, `start`, `stop`, `GPIO1` or `GPIO2`.
- Inspect header/footer text and PAGE field separately because `Document.paragraphs` does not cover them.
- The output contains no text boxes, comments, tracked changes, content controls, footnotes or endnotes.

## Stable locators

- Main body: `/word/document.xml/w:document/w:body`.
- Section properties: final `/word/document.xml/w:document/w:body/w:sectPr`.
- Default and first-page headers/footers: relationships from `word/_rels/document.xml.rels`; both pairs carry identical text, rule and PAGE field treatment.
- Style roles: `Normal`, `Heading 1`, `Heading 2`, `Heading 3`, `Caption`, and optional `Table Title` in `/word/styles.xml`.
- New diagram relationship: the `a:blip/@r:embed` referenced by the paragraph immediately preceding caption `图 3.X-1`.

## Package preservation

- Preserve-only: `[Content_Types].xml` entries unrelated to removed/new body media; `_rels/.rels`; `word/styles.xml` existing style definitions; `word/settings.xml`; `word/theme/theme1.xml`; `word/fontTable.xml`; `word/webSettings.xml`; `word/header1.xml`; `word/footer1.xml`; header/footer relationships; docProps application metadata except timestamps.
- Editable: `word/document.xml`; `word/_rels/document.xml.rels`; `word/media/*`; core title/author properties; optional new table-title style/numbering definition.
- Remove-only: obsolete visual-module media relationships and media parts, if the save library removes them.
- There are no custom XML parts, comments, tracked changes or content controls in the retained reference.

## Fidelity gates

- The retained reference still matches its recorded SHA-256 after authoring.
- Final section count remains one and A4 geometry/margins match this contract.
- Header rule, header text, footer PAGE field, typography hierarchy and paragraph rhythm remain recognizably source-derived.
- All tables use explicit widths and cell margins; no clipped or pinned cell text.
- The functional diagram remains legible at 100% zoom and its caption stays attached.
- Page breaks avoid nearly blank pages, split heading lines or isolated headings; `4.X` may begin after the preceding table.
- No communication/integration content and no stale visual-module text or images remain.
- Final Word-exported page images are inspected page by page at 100% zoom. Any clipping, overlap, broken table, missing glyph or poor page break must be corrected and re-exported.
