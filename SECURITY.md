
┌────────────────────┬────────────────────┐
│ Version            │ Supported          │
├────────────────────┼────────────────────┤
│ latest             │ :white_check_mark: │
├────────────────────┼────────────────────┤
│ n-1                │ :white_check_mark: │
├────────────────────┼────────────────────┤
│ older              │ :x:                │
└────────────────────┴────────────────────┘


Notes:
- “latest” is the most recent tagged stable release.
- “n-1” is the previous minor release, if still in use.
- Older versions receive best-effort guidance only.

Reporting a Vulnerability

How to report
- Open a private GitHub Security Advisory in this repository:
  - Go to the repository Security tab
  - Click Advisories
  - Click Report a vulnerability
- If you need to share encrypted details, use the project PGP key:
  - Key URL: https://raw.githubusercontent.com/MedorCoin-Blockchain/main/keys/medorcoin-public.asc
  - Fingerprint: TO-BE-FILLED

What to include
- Affected component and version or commit hash
- Impact (confidentiality, integrity, availability)
- Reproduction steps or proof-of-concept
- Environment (OS, compiler, configuration)
- Any suggested remediation

Response targets
- Acknowledgement within 72 hours
- Initial assessment within 7 days
- Fix or mitigation ETA communicated within 30 days for high/critical issues

Coordinated disclosure
- Please allow up to 90 days for investigation, patching, and release.
- We may credit reporters (with consent) in release notes/advisories.
- Avoid public disclosure until a patched release or the agreed date.
