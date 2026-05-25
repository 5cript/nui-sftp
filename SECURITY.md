# Security Policy

## Supported Versions

Only the latest released version of nui-sftp receives security updates.
Older versions are not maintained, as nui-sftp does not currently offer long-term support releases.

| Version | Supported          |
| ------- | ------------------ |
| latest  | :white_check_mark: |
| < latest | :x:               |

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Report vulnerabilities privately through GitHub's [private vulnerability
reporting](https://github.com/5cript/nui-sftp/security/advisories/new) feature.
This keeps the report confidential until a fix is released.

If you cannot use GitHub's private reporting, you may instead contact the
maintainer directly. See the contact information in the repository profile.

### What to include

A useful report typically contains:

- A description of the vulnerability and its potential impact
- Steps to reproduce, or a proof-of-concept
- The affected version and platform. (If not the public release build include the build configuration)
- Any suggested mitigation, if you have one

### What to expect

- **Acknowledgment:** within 7 days of your report.
- **Initial assessment:** within 14 days, including whether the report is
  accepted as a security issue or treated as a regular bug.
- **Fix and disclosure:** for accepted reports, I aim to release a fix
  within 14 days of confirmation. Complex issues may take longer; I will
  keep you updated on progress.
- **Credit:** with your permission, reporters are credited in the published
  security advisory and the changelog.

As nui-sftp is maintained by a single person, these are best-effort
timelines rather than guarantees. Critical issues are prioritized.

### Coordinated disclosure

Please give me a reasonable window to release a fix before disclosing
the vulnerability publicly. Once a patched release is available, a
GitHub security advisory will be published with details and credit.

### Out of scope

The following are generally not considered security vulnerabilities:

- Issues in third-party dependencies (please report those upstream;
  I'll update once a fix is available)
- Bugs that require physical access to an already-compromised system
- Theoretical issues without a demonstrated impact
