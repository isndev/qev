#!/usr/bin/env python3
"""Render the GitHub release body for a tag, from CHANGELOG.md.

Typed release notes are unreproducible: nothing ties them to the commit being tagged, and a
re-run of the release workflow replaces them with whatever it was handed -- which, the first
time round, was nothing at all. So the notes come from the file that is reviewed with the
change, and a tag whose version has no section here fails the release instead of publishing
an empty page.

This lives in a script rather than inside the workflow YAML on purpose. A heredoc embedded in
a `run:` block is processed by three layers before Python sees it -- GitHub's ${{ }}
substitution, the YAML block scalar, then the shell -- and it cannot be run locally, which is
where a mistake in it would be found.

    python3 scripts/release-notes.py 5.0.0 > RELEASE_NOTES.md
"""
import os
import pathlib
import re
import sys

MIN_BODY = 200  # a section shorter than this is a heading somebody forgot to fill in


def render(version: str, changelog: pathlib.Path, repo: str) -> str:
    text = changelog.read_text(encoding="utf-8")
    match = re.search(r"^## \[" + re.escape(version) + r"\].*?$(.*?)(?=^## \[|\Z)",
                      text, re.S | re.M)
    if match is None:
        sys.exit(f"{changelog}: no section for {version} -- add one before tagging")
    body = match.group(1).strip()
    if len(body) < MIN_BODY:
        sys.exit(f"{changelog}: the {version} section is {len(body)} characters, "
                 f"which is not release notes")
    return f"""{body}

---

### Verifying a download

```sh
sha256sum -c SHA256SUMS
gh attestation verify qev-{version}-Linux-qev_Runtime.tar.gz --repo {repo}
```

Every archive is built by GitHub Actions from this tag and carries a signed build-provenance
attestation.
"""


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <version>")
    version = sys.argv[1].lstrip("v")
    # A version, not just any heading. Without this the [Unreleased] section renders happily
    # into a release body -- the tag pattern in release.yml would not let such a tag through,
    # but a check that relies on a caller elsewhere being careful is not a check.
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        sys.exit(f"{version!r} is not a MAJOR.MINOR.PATCH version")
    root = pathlib.Path(__file__).resolve().parent.parent
    repo = os.environ.get("GITHUB_REPOSITORY", "isndev/qev")
    sys.stdout.write(render(version, root / "CHANGELOG.md", repo))


if __name__ == "__main__":
    main()
