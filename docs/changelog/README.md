# Changelog site

The changelog at <https://gfhdhytghd.github.io/hymission/> is generated from
`releases.json` in this directory. The data is the single source of truth;
`docs/index.html` is generated output, committed so the page can be previewed
without building it.

| File | Purpose |
| --- | --- |
| `releases.json` | Release entries (newest first) and the Unreleased section |
| `template.html` | Page layout and inline CSS |
| `build.py` | Validates the data and writes `docs/index.html` (Python 3 standard library only) |
| `../../.github/workflows/pages.yml` | Checks that the page is current and deploys it to GitHub Pages |

## Updating

While developing, add user-facing changes to `unreleased.sections`.

When publishing a release:

1. Publish the GitHub release first, so its UTC `published_at` timestamp exists.
2. Move the Unreleased items into a new entry at the **top** of `releases`:

   ```json
   {
     "tag": "v0.9.0-v0.56.2",
     "version": "0.9.0",
     "hyprland": "0.56.2",
     "published_at": "2026-10-20T12:00:00Z",
     "headline": "One-line theme of the release",
     "sections": [{ "title": "New", "items": ["..."] }]
   }
   ```

   Copy `published_at` exactly from GitHub:
   `gh api repos/gfhdhytghd/hymission/releases/tags/<tag> --jq .published_at`.
3. Set `unreleased.since` to the new tag and keep only still-pending items, or
   delete the `unreleased` object if there are no pending changes.
4. Regenerate and verify:

   ```sh
   python3 docs/changelog/build.py
   gh api repos/gfhdhytghd/hymission/releases --paginate \
     | python3 docs/changelog/build.py --verify-github -
   ```

5. Commit `releases.json` and `docs/index.html` together. Pushing to `master`
   deploys the site; the workflow fails if `docs/index.html` is stale.

Items accept `` `code` `` spans, and `#123` links to the matching issue or pull request.
Write for users: describe what changed and why it matters, not commit subjects.

## One-time setup

In the repository settings, set **Pages → Build and deployment → Source** to
**GitHub Actions**.

Historical tags use both `-v<Hyprland>` and `-<Hyprland>` suffixes. The initial
`v0.0.1` release has no declared Hyprland target; its `hyprland` value is `null`
rather than an inferred compatibility claim.
