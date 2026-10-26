# The 2026 landscape for large CSV files

Why Tessera exists, stated as a market observation rather than a preference. Every claim below is
sourced to the vendor's or project's own documentation, checked in 2026. Where a page does not state
something, this document says so instead of filling the gap.

## The structural problem

**Spreadsheets cannot hold the files people actually receive.**

Microsoft's own specification caps an Excel worksheet at *"1,048,576 rows by 16,384 columns"*
([Excel specifications and limits](https://support.microsoft.com/en-us/excel/excel-specifications-and-limits)).
Microsoft's guidance for files beyond that cap is blunt about the consequence of saving:
*"If you save this workbook, you'll lose data that wasn't loaded."*
([what to do if a data set is too large for the Excel grid](https://support.microsoft.com/en-us/excel/what-to-do-if-a-data-set-is-too-large-for-the-excel-grid)).

Google Sheets is capped at *"up to 20 million cells"* per spreadsheet
([Google Drive Help](https://support.google.com/drive/answer/37603)), doubled from 10 million in 2026
([Google Workspace Updates](https://workspaceupdates.googleblog.com/2026/09/doubled-cell-limits-in-google-sheets-now-generally-available.html)).
Twenty million cells is roughly 1.5 million rows of a 13-column file — and the page states the limit
without stating what the product does as you approach it.

A single 1 GB export, which is an ordinary monthly billing or event export, is outside both.
The most widely installed data tools in the world cannot open the file, and the file is not unusual.

## The options, honestly described

| Tool | Big files | Where your data goes | Setup | Price | License |
|---|---|---|---|---|---|
| Excel / Google Sheets | capped by design | Google Sheets: Google's servers | none | included / free tier | proprietary |
| Gigasheet | yes | vendor storage — *"Original uploaded files"* and *"Parsed uploaded files"* in AWS data centres ([security](https://www.gigasheet.com/security)) | none | tiers listed; dollar amounts not shown on the pricing page ([pricing](https://www.gigasheet.com/pricing)) | proprietary |
| Row Zero | yes | vendor storage — *"We retain data on your behalf, such as workbooks"* ([privacy](https://rowzero.com/privacy-policy)) | none | Free $0 · Pro $12/user/mo · Business $20/user/mo ([pricing](https://rowzero.com/pricing)) | proprietary |
| Tad | yes | not stated on the site | download | described as *"free, cross-platform desktop application"* ([tadviewer.com](https://www.tadviewer.com/)) | MIT ([LICENSE](https://github.com/antonycourtney/tad/blob/master/LICENSE)) |
| pandas / R | yes, if it fits in RAM | local | Python/R + packages | free | BSD / GPL |
| DuckDB | yes | local | download | free | MIT |
| csvkit | yes | local | Python ≥3.10 ([pyproject.toml](https://github.com/wireservice/csvkit/blob/master/pyproject.toml)) | free | MIT |
| VisiData | yes | local | Python 3.8+ ([GitHub](https://github.com/saulpw/visidata)) | free | GPL |
| **Tessera** | yes | **local — never leaves the machine** | **one file, none** | free | MIT |

## Where each one loses

**Cloud spreadsheets lose on confidentiality, before anything else.** Gigasheet's security page
documents that uploaded files are *stored* by the service, and Row Zero's privacy policy documents
that it *retains workbooks on your behalf*. That is a normal, disclosed SaaS model, and for a
marketing team it is a fine trade. It is not a decision an engineer can make for patient records,
payroll, security telemetry, legal discovery, or anything covered by a customer NDA. Those are
precisely the files that are large, and precisely the files that are not allowed to leave the laptop
they arrived on. Cloud tools cannot win this category at any price, because the objection is not
price.

**Notebook and scripting stacks lose on weight and on the first step.** pandas describes its data
structures as for *"in-memory analytics"* ([scaling guide](https://pandas.pydata.org/docs/user_guide/scale.html)),
which is exactly right and exactly the problem: on the 1.09 GiB benchmark file, `read_csv` measured
**34.8 seconds and 5.36 GB of RAM** before a single question could be asked (see
[BENCHMARKS.md](BENCHMARKS.md)). It also requires a Python environment, and in practice a virtualenv
and a dependency tree. For a one-off "what is in this file", that is a large amount of ceremony, and
the machine may not have the RAM at all.

**DuckDB loses on the first-30-seconds experience, and wins on everything after.** DuckDB's
documented way to look at a CSV is `SELECT * FROM 'flights.csv';`
([CSV import docs](https://duckdb.org/docs/2.0/data/csv/overview.html)) — you get a real database,
which is what you want the moment the question becomes analytical. But "look at this file" is not an
analytical question, and the documented route to it is SQL. Tessera's entire pitch is that the grid
appears first and SQL is available when you want it, not required before you can see row one.

**Python TUIs lose on installation.** csvkit and VisiData are both Python applications requiring a
modern Python ([csvkit](https://github.com/wireservice/csvkit/blob/master/pyproject.toml),
[VisiData](https://github.com/saulpw/visidata)), which is fine on a developer's laptop and awkward on
a jump box, a locked-down analyst workstation, or a colleague's machine that has never had a
toolchain. VisiData is a genuinely great program; the friction is not the interface, it is the runtime.

**Tad** is the closest thing to a competitor: a free, open-source, cross-platform desktop viewer.
It is a good tool, and the honest difference is that Tessera is a terminal program with a scriptable
CLI, no runtime beyond the OS, and a keyboard-first interface — a different shape for a different
user, not a claim that Tad is bad.

## The gap

No widely used option is simultaneously:

1. able to open files far larger than RAM, and
2. entirely local and offline by construction, and
3. a single executable with no runtime, installer or account, and
4. **immediately** usable with no query language required before you can see the data, and
5. free and open source.

Each existing tool gives up at least one of those, and the one each gives up is the one its user
matters most about. Tessera is aimed precisely at that intersection: the moment when someone has a
file too big for Excel, from a source they are not allowed to upload, on a machine with no toolchain,
and they need to see what is in it right now.

## What would make this thesis wrong

Worth stating, so the argument can be checked rather than believed:

* If Microsoft or Google removed the size ceilings, the mass-market half of this problem goes away.
  (Neither shows any sign of doing so; the limits are architectural.)
* If DuckDB or pandas became a zero-install single binary with a grid front end, Tessera's
  differentiation would collapse to polish. DuckDB's CLI is a single binary already; it is the
  "look first, query later" experience that is missing there, not the engine.
* If offline handling became unnecessary because every environment permitted uploading customer data
  to a third party, the privacy argument disappears. Nothing in 2026 suggests that direction.

The bet this project makes is that "large file, cannot upload, no toolchain, need to look now" is a
recurring situation for enough people that a small, fast, offline tool earns its place — and that the
same people will appreciate having a real query engine when they stop looking and start asking.
