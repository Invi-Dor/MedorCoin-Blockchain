
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
 
Release Immutability
- Policy: Published releases (including their tags, titles/bodies, assets, and checksums) are immutable. No edits or replacements after publication.
- Rationale: Prevents tampering or drift, preserves reproducibility, and ensures signatures/verifications remain valid.
- Process for changes: If corrections are needed, publish a new release (e.g., v1.2.1) from the appropriate commit; do not edit or re-upload assets to an existing release.
- Verification: Each release is signed; users should verify signatures and checksums against the original publication.

Maintainer checklist to enforce immutability

One-time repository settings
- Enforce signed tags:
  - Require annotated, GPG-signed tags for releases.
- Protect main/release branches:
  - Require pull requests, status checks, and signed commits on protected branches.
  - Restrict who can push tags and who can create GitHub releases.
- Lock down releases:
  - Limit “Publish release” permission to a small maintainer group.
  - Disable “Allow edits from maintainers” on release PRs if it risks asset churn.
- Enable checks:
  - Configure CI to publish releases only from immutable artifacts (reproducible build job).
  - Fail CI if an existing tag already exists or if a release with the same tag is found.

Operational workflow
- Tagging:
  - Create an annotated, signed tag (e.g., git tag -s v1.2.0 -m "MedorCoin v1.2.0").
  - Push tags with git push --tags (or specific tag).
- Artifact build:
  - Build artifacts in CI from the tagged commit; record build metadata (compiler, OS, hashes).
  - Generate checksums and a signed checksum file (e.g., SHA256SUMS and SHA256SUMS.asc).
- Publish:
  - Create a new GitHub Release from the signed tag.
  - Upload artifacts, checksums, and signature files once. Double-check hashes before publishing.
- No edits:
  - Do not modify the release body, assets, or attached files after publishing.
  - Never delete and recreate a tag to “fix” a release. Cut a new patch version instead.
- Corrections procedure:
  - If an error is discovered, publish a new release (e.g., v1.2.1) with corrected artifacts.
  - Add a deprecation note to the old release body (single appended note allowed if policy permits text-only clarifications; otherwise leave as-is and announce via a separate advisory).
  - Announce changes in CHANGELOG and security/advisory channels.

Monitoring and auditing
- Subscribe maintainers to release and tag activity notifications.
- Periodically export release metadata (titles, assets, checksums) and store in an append-only log (e.g., artifact manifest in a separate repo or Object Storage with versioning).
- Verify that release assets’ hashes match the published checksum file; alert on drift.

User guidance (optional snippet for README)
- Every release is immutable. If you find differences between your download and the published checksums/signatures, do not use the artifacts. Open a private GitHub security advisory with details.

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
