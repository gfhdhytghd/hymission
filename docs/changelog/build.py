#!/usr/bin/env python3
"""Validate docs/changelog/releases.json and render docs/index.html.

Usage:
  python3 docs/changelog/build.py            # write docs/index.html
  python3 docs/changelog/build.py --check    # fail if docs/index.html is stale
  gh api repos/gfhdhytghd/hymission/releases --paginate \
    | python3 docs/changelog/build.py --verify-github -

Only the Python standard library is required.
"""

import argparse
import datetime
import html
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "releases.json"
TEMPLATE = HERE / "template.html"
OUTPUT = HERE.parent / "index.html"

TIMESTAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:\.\d+)?$")
HYPRLAND_RE = re.compile(r"^\d+\.\d+(\.\d+)?$")
INLINE_RE = re.compile(r"`([^`]+)`|(?<![\w&/])#(\d+)\b")


class DataError(Exception):
    pass


def fail_unless(condition, message):
    if not condition:
        raise DataError(message)


def validate_sections(sections, where):
    fail_unless(isinstance(sections, list) and sections, f"{where}: sections must be a non-empty list")
    for section in sections:
        fail_unless(isinstance(section.get("title"), str) and section["title"].strip(), f"{where}: section without title")
        items = section.get("items")
        fail_unless(isinstance(items, list) and items, f"{where}: section '{section['title']}' has no items")
        for item in items:
            fail_unless(isinstance(item, str) and item.strip(), f"{where}: empty item in '{section['title']}'")
            fail_unless(item.count("`") % 2 == 0, f"{where}: unbalanced backticks in: {item}")


def validate(data):
    for key in ("project", "repository", "site_url"):
        fail_unless(isinstance(data.get(key), str) and data[key], f"missing '{key}'")
    releases = data.get("releases")
    fail_unless(isinstance(releases, list) and releases, "'releases' must be a non-empty list")

    seen = set()
    previous = None
    for release in releases:
        tag = release.get("tag", "")
        where = f"release {tag or '?'}"
        fail_unless(tag not in seen, f"{where}: duplicate tag")
        seen.add(tag)
        fail_unless(VERSION_RE.match(release.get("version", "")), f"{where}: version must look like 1.2.3 or 1.2.3.4")
        fail_unless(release.get("hyprland") is None or HYPRLAND_RE.match(release.get("hyprland", "")), f"{where}: hyprland must look like 0.56 or 0.56.2")
        expected = {f"v{release['version']}-{release['hyprland']}", f"v{release['version']}-v{release['hyprland']}"} if release['hyprland'] else {"v0.0.1"}
        fail_unless(tag in expected, f"{where}: tag does not match version and Hyprland metadata")
        published = release.get("published_at", "")
        fail_unless(TIMESTAMP_RE.match(published), f"{where}: published_at must be UTC like 2026-01-31T12:00:00Z")
        datetime.datetime.strptime(published, "%Y-%m-%dT%H:%M:%SZ")
        fail_unless(isinstance(release.get("headline"), str) and release["headline"].strip(), f"{where}: missing headline")
        validate_sections(release.get("sections"), where)
        if previous is not None:
            fail_unless(published <= previous, f"{where}: releases must be ordered newest first")
        previous = published

    unreleased = data.get("unreleased")
    if unreleased is not None:
        fail_unless(unreleased.get("since") == releases[0]["tag"],
                    f"unreleased.since must be the newest release tag ({releases[0]['tag']})")
        validate_sections(unreleased.get("sections"), "unreleased")


def inline(text, repo_url):
    """Escape text, render `code` spans and link #123 references."""
    out = []
    last = 0
    for match in INLINE_RE.finditer(text):
        out.append(html.escape(text[last:match.start()]))
        if match.group(1) is not None:
            out.append(f"<code>{html.escape(match.group(1))}</code>")
        else:
            number = match.group(2)
            out.append(f'<a href="{repo_url}/issues/{number}">#{number}</a>')
        last = match.end()
    out.append(html.escape(text[last:]))
    return "".join(out)


def render_sections(sections, repo_url, indent):
    pad = " " * indent
    parts = []
    for section in sections:
        parts.append(f"{pad}<h3>{html.escape(section['title'])}</h3>")
        parts.append(f"{pad}<ul>")
        parts.extend(f"{pad}  <li>{inline(item, repo_url)}</li>" for item in section["items"])
        parts.append(f"{pad}</ul>")
    return "\n".join(parts)


def anchor(tag):
    return re.sub(r"[^a-z0-9-]+", "-", tag.lower()).strip("-")


def render(data):
    repo_url = f"https://github.com/{data['repository']}"
    releases = data["releases"]
    nav = []
    entries = []

    unreleased = data.get("unreleased")
    if unreleased:
        compare = f"{repo_url}/compare/{unreleased['since']}...master"
        nav.append('      <li><a href="#unreleased">Unreleased <span>next</span></a></li>')
        entries.append(f"""      <li>
        <article class="entry unreleased" id="unreleased" aria-labelledby="unreleased-title">
          <div class="entry-head">
            <h2 id="unreleased-title"><a href="#unreleased">Unreleased</a></h2>
            <div class="meta"><span class="badge pending">Not yet released</span><span>Since {html.escape(unreleased['since'])}</span></div>
          </div>
          <p class="summary">{inline(unreleased['summary'], repo_url)}</p>
{render_sections(unreleased['sections'], repo_url, 10)}
          <div class="entry-foot"><a href="{compare}">Compare {html.escape(unreleased['since'])}…master</a></div>
        </article>
      </li>""")

    for index, release in enumerate(releases):
        tag = release["tag"]
        tag_html = html.escape(tag)
        slug = anchor(tag)
        date = release["published_at"][:10]
        day = datetime.date.fromisoformat(date)
        pretty = f"{day:%b} {day.day}, {day.year}"
        url = f"{repo_url}/releases/tag/{tag}"
        latest = '<span class="badge latest">Latest</span>' if index == 0 else ""
        nav.append(f'      <li><a href="#{slug}">{html.escape(release["version"])} <span>{date}</span></a></li>')

        foot = [f'<a href="{url}">Release notes for {tag_html} on GitHub</a>']
        if index + 1 < len(releases):
            older = releases[index + 1]["tag"]
            foot.append(f'<a href="{repo_url}/compare/{older}...{tag}">Compare with {html.escape(older)}</a>')
        else:
            foot.append(f'<a href="{repo_url}/tree/{tag}">Browse source at {tag_html}</a>')

        entries.append(f"""      <li>
        <article class="entry" id="{slug}" aria-labelledby="{slug}-title">
          <div class="entry-head">
            <h2 id="{slug}-title"><a href="#{slug}">{html.escape(release['version'])}</a></h2>
            <div class="meta">{latest}<time datetime="{release['published_at']}">{pretty}</time><span class="badge">Hyprland {html.escape(release['hyprland'] or 'not specified')}</span><code>{tag_html}</code></div>
          </div>
          <p class="headline">{inline(release['headline'], repo_url)}</p>
{render_sections(release['sections'], repo_url, 10)}
          <div class="entry-foot">{''.join(foot)}</div>
        </article>
      </li>""")

    latest = releases[0]
    values = {
        "PROJECT": html.escape(data["project"]),
        "SITE_URL": html.escape(data["site_url"]),
        "REPO_URL": repo_url,
        "LATEST_TAG": html.escape(latest["tag"]),
        "LATEST_URL": f"{repo_url}/releases/tag/{latest['tag']}",
        "NAV": "\n".join(nav),
        "ENTRIES": "\n".join(entries),
    }
    page = TEMPLATE.read_text(encoding="utf-8")
    for key, value in values.items():
        page = page.replace("{{" + key + "}}", value)
    leftover = re.findall(r"\{\{[A-Z_]+\}\}", page)
    fail_unless(not leftover, f"unfilled template placeholders: {leftover}")
    return page


def load_json_stream(text):
    """Parse `gh api --paginate` output, which may be several JSON arrays back to back."""
    decoder = json.JSONDecoder()
    items, pos = [], 0
    while pos < len(text):
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text):
            break
        value, pos = decoder.raw_decode(text, pos)
        items.extend(value if isinstance(value, list) else [value])
    return items


def verify_github(data, source):
    text = sys.stdin.read() if source == "-" else Path(source).read_text(encoding="utf-8")
    published = {r["tag_name"]: r for r in load_json_stream(text) if not r.get("draft")}
    ours = {r["tag"]: r for r in data["releases"]}
    problems = []
    for tag in sorted(set(published) - set(ours)):
        problems.append(f"missing from releases.json: {tag} (published {published[tag]['published_at']})")
    for tag in sorted(set(ours) - set(published)):
        problems.append(f"not a published GitHub release: {tag}")
    for tag in sorted(set(ours) & set(published)):
        if ours[tag]["published_at"] != published[tag]["published_at"]:
            problems.append(f"{tag}: published_at {ours[tag]['published_at']} != GitHub {published[tag]['published_at']}")
        if published[tag].get("prerelease"):
            problems.append(f"{tag}: marked as prerelease on GitHub")
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if not problems:
        print(f"ok: {len(ours)} releases match GitHub tags and published_at timestamps")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if the generated page is out of date")
    parser.add_argument("--verify-github", metavar="FILE", help="compare against `gh api .../releases` JSON ('-' for stdin)")
    parser.add_argument("--output", type=Path, default=OUTPUT, help="output path (default: docs/index.html)")
    args = parser.parse_args()

    try:
        data = json.loads(DATA.read_text(encoding="utf-8"))
        validate(data)
        page = render(data)
    except (DataError, ValueError, KeyError) as error:
        print(f"error: {DATA.name}: {error}", file=sys.stderr)
        return 1

    if args.verify_github:
        return 0 if verify_github(data, args.verify_github) else 1

    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != page:
            print(f"error: {args.output} is out of date; run python3 docs/changelog/build.py", file=sys.stderr)
            return 1
        print(f"ok: {args.output} is up to date")
        return 0

    args.output.write_text(page, encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
