# Releasing Idle Dimmer

## What becomes a release?

A commit is a saved snapshot of the source. A branch such as `main` moves forward
as commits are added. A version tag such as `v1.0.1` identifies the snapshot used
for a release. The release adds a download page, notes, and compiled files to that tag.

Ordinary pushes and pull requests keep running `.github/workflows/build.yml`.
They do not publish releases. The separate **Prepare release** workflow runs only
when someone with repository write access deliberately starts it, and only from `main`.
It uses the exact commit selected when that run starts, even if `main` advances later.

## Delegate a release

Ask the coding agent: **"Prepare release 1.0.2 from main."**

The release work is:

1. Finish and review the changes to include. Bump `IDLE_DIMMER_VERSION` in
   `AppVersion.h`, update the README version, and write brief user-facing highlights
   in `docs/releases/v1.0.2.md`. Commit and push the reviewed source to `main`.
2. Run **Actions → Prepare release → Run workflow**, select `main`, and enter
   `1.0.2` without the leading `v`. The equivalent CLI command is:

   ```powershell
   gh workflow run release.yml --ref main -f version=1.0.2
   ```

3. Wait for the workflow. It validates the version, compiles, runs the app self-test,
   packages the explicit release files, and creates a **draft** release. The Actions
   summary links to it. A failed build or test stops before draft creation.
4. Review the draft's notes and downloads. Publish it from GitHub Releases when ready,
   or explicitly delegate publication to the agent. Until publication, the draft is
   not a public release and does not become an update for users.

The workflow must first be committed and pushed to the default branch before its
Run workflow button becomes available. See [GitHub's manual workflow instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).

There is no release on each commit, no release from a feature branch, and no overwrite
of an existing version. If a draft already exists for a version, review that draft;
do not rerun the workflow to replace its assets. If draft creation partly fails,
inspect the draft and the preserved Actions package before retrying.

## Release notes

`tools/prepare-release.ps1` combines the optional short highlights file with an
automatic list of commit subjects and links since the previous published stable
release. Direct commits are included, even when no pull requests were used.
It also records the full source commit and a comparison link.

GitHub appends its generated PR/contributor notes through `--generate-notes`.
`.github/release.yml` groups PRs by labels such as `enhancement`, `bug`, and
`documentation`; unlabelled PRs fall under Other changes. `skip-changelog` excludes
a PR from GitHub's PR summary, but deliberately does not hide it from the complete
commit list. See [GitHub's release-note documentation](https://docs.github.com/en/repositories/releasing-projects-on-github/automatically-generated-release-notes).

You do not need to write the commit list by hand. Good commit subjects make it
more useful. The brief highlights are an editorial summary of the actual changes;
an agent can write those when preparing the release. No paid AI service or API key
is used by the workflow.

## Downloads and checksums

Each draft attaches:

- `IdleDimmer.exe`: the standalone application.
- `IdleDimmer-<version>-windows-x64.zip`: only the EXE and MIT licence.
- `SHA256SUMS.txt`: SHA-256 hashes for the EXE and ZIP.

The package never sweeps the working directory. Settings, statistics, brightness
recovery files, logs, screenshots, and build tools are excluded by the explicit
file list. The Actions artifact also contains the generated notes for inspection.
Checksums detect a mismatched or damaged download; they are not a publisher signature.
The application remains unsigned.

The footer reads `AppVersion.h`. Packaging checks that the requested version matches
that header and the README, and the workflow rejects an existing tag or a version
that is not newer than the previous stable release.

For a **local preview only**, after building and running the self-test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/prepare-release.ps1 -Version 1.0.1 -PreviousTag v1.0.0 -AllowDirty
```

Output goes to ignored `build-tools/release/`. `-AllowDirty` marks the notes as a
preview because uncommitted changes cannot appear in the commit list. Never publish
that preview as a release; the GitHub workflow packages a clean checkout instead.
The script neither launches the app nor publishes anything. `-ValidateOnly` checks
version inputs without requiring a built EXE or creating output.

## Scope and validation

UNVERIFIED 2026-11-02: the new workflow has not yet run on GitHub. Local build,
self-test, packaging, checksum and ZIP inspection results are reported with the change.
MEASURED 2026-11-02: local build/doc/icon gates passed, app self-test passed with
2,368 checks, and the rebuilt app was relaunched on the interactive desktop.
The package contained only the EXE and licence; the archived EXE matched the tested
binary and both published-file checksum calculations matched. Invalid and mismatched
versions, invalid previous-tag syntax, and dirty packaging without `-AllowDirty`
were rejected. Workflow YAML and embedded PowerShell blocks parsed successfully.
This setup adds release preparation only. In-app update detection and installation
are deferred at the user's request; it adds no network activity or background work
to the running application.

Standard GitHub-hosted runners are free for public repositories under the pricing
checked 2026-11-02. This workflow uses `windows-latest`, without paid larger runners.
See [GitHub Actions billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions).
