# Security Policy

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security bugs. Use one
of these private channels instead:

- **Preferred:** GitHub's private vulnerability reporting, via the
  "Report a vulnerability" button on the [Security tab](../../security)
  of this repo. This creates a confidential advisory thread visible
  only to maintainers and you.
- **Email:** `viggo@lekdorf.com` with `[praia security]` in the subject.

Please include:

- A description of the issue and its impact.
- Steps or a minimal script that reproduces the problem.
- The affected Praia version and platform.

## Response timeline

We aim to solve all security issues within 30 days.

If a fix needs longer than 30 days (deep architectural issue, upstream
dependency, etc.) we'll let you know with a revised timeline.

## Supported versions

Pre-1.0 Praia ships fixes on `main` and in the most recent tagged
release only. Older tagged releases are not patched — upgrade to the
latest release.

## Scope

In scope:

- The `praia` interpreter and bytecode VM (parser, evaluator, GC).
- Standard library grains and built-in modules (crypto, http, net,
  fs, json, yaml, etc.).
- The `sand` package manager.
- Native plugin loading.

Out of scope:

- Third-party grains published outside this repository.
- Vulnerabilities in transitive dependencies (OpenSSL, re2, etc.) —
  please report those upstream; if a wrapper layer can mitigate a
  known CVE we'll consider that in scope.
- Issues that require an attacker to already have local code execution
  or write access to the script being run.

## Disclosure

We follow coordinated disclosure: the reporter and maintainer agree on
a public disclosure date after a fix ships, typically within 90 days
of the initial report. Credit is given in the release notes unless the
reporter requests otherwise.
