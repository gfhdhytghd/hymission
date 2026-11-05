#!/usr/bin/env python3
"""Scheduled maintenance for the user's Hyprland plugin repositories (repos.json).

`sync` merges each default branch into its long-lived sibling branches through the same gates.
Each open issue or pull request whose content changed since the last handled snapshot gets its own isolated
clone and a Claude run that may comment, push to any branch, merge and close, subject to the gates in
instructions.txt. The user's own checkouts are never edited; after a run this script only fast-forwards the
local branches the agent published, when that is safe."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path('/home/wilf/.local/share/hyprland-plugins-maintenance')
STATE = Path('/home/wilf/.local/state/hyprland-plugins-maintenance')
CONFIG = json.loads((ROOT / 'repos.json').read_text())
OWNER = CONFIG['owner']
LOGFILE = STATE / 'maintenance.log'
MODE = '-'
# Nothing started from here may find the live compositor: the timer inherits the session's variables.
SCRUB = ('WAYLAND_DISPLAY', 'DISPLAY', 'HYPRLAND_INSTANCE_SIGNATURE', 'SWAYSOCK', 'HYPRLAND_CMD', 'XDG_SESSION_ID', 'LD_PRELOAD')


def log(message):
    print(message, flush=True)
    try:
        STATE.mkdir(parents=True, exist_ok=True)
        if LOGFILE.exists() and LOGFILE.stat().st_size > 5 * 1024 * 1024:
            LOGFILE.replace(LOGFILE.with_suffix('.log.1'))
        stamp = datetime.datetime.now().astimezone().isoformat(timespec='seconds')
        with LOGFILE.open('a') as output:
            for line in str(message).splitlines() or ['']:
                output.write(f'{stamp} [{MODE}] {line}\n')
    except OSError:
        pass


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=None).strip()


def api(endpoint):
    return json.loads(run('gh', 'api', endpoint))


def paginate(endpoint):
    pages = json.loads(run('gh', 'api', '--paginate', '--slurp', endpoint))
    return [item for page in pages for item in page]


def slug(repo):
    return f'{OWNER}/{repo}'


def snapshot(repo, item):
    number = item['number']
    path = f'repos/{slug(repo)}'
    result = {'issue': api(f'{path}/issues/{number}'),
              'comments': paginate(f'{path}/issues/{number}/comments?per_page=100')}
    if 'pull_request' in item:
        result['pull'] = api(f'{path}/pulls/{number}')
        for kind in ('reviews', 'comments'):
            result[kind + '_pr'] = paginate(f'{path}/pulls/{number}/{kind}?per_page=100')
    return result


def notify(summary, body='', urgency='normal'):
    subprocess.run(['notify-send', '--app-name=Hyprland plugins', f'--urgency={urgency}', summary, body[:500]],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def latest_release():
    return api('repos/hyprwm/Hyprland/releases/latest')['tag_name']


def branch_targets(repo, checkout):
    """Hyprland revision each branch of this repository must build against, as hyprland-prefix.sh arguments."""
    rows = []
    release = None
    for line in run('git', 'branch', '-r', '--format=%(refname:short)', cwd=checkout).splitlines():
        if '/' not in line or line.endswith('/HEAD'):
            continue
        branch = line.split('/', 1)[1]
        target = CONFIG['branch_targets'].get(branch)
        if not target:
            rows.append(f'{branch}: no configured target; treat as a topic branch of the branch it forked from')
            continue
        hypr_slug, hypr_ref = target['hyprland']
        lock = subprocess.run(['git', 'show', f'origin/{branch}:flake.lock'], cwd=checkout, capture_output=True, text=True)
        pinned = None
        if lock.returncode == 0:
            node = json.loads(lock.stdout)['nodes'].get('hyprland', {})
            pinned = (node.get('original', {}).get('ref') or node.get('locked', {}).get('rev'))
        if hypr_ref == 'flake-lock':
            hypr_ref = pinned or 'v?.?.? (no flake.lock on this branch: read its README)'
        elif hypr_ref == 'latest-release':
            release = release or latest_release()
            hypr_ref = pinned or release
            if pinned and pinned != release:
                hypr_ref = f'{pinned} (flake pin; official latest is {release}: a bump is a separate tier-3 change)'
        text = f'{branch}: hyprland-prefix.sh {hypr_slug} {hypr_ref}'
        if 'aquamarine' in target:
            text += ' --aquamarine {} {}'.format(*target['aquamarine'])
        rows.append(text)
    return '\n'.join(rows)


def workspace(label, repo):
    target = STATE / 'runs' / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + label)
    target.mkdir(parents=True)
    checkout = target / 'repo'
    local = Path(CONFIG['repos'][repo]['local'])
    run('git', 'clone', '--quiet', '--reference-if-able', str(local), '--dissociate', f'https://github.com/{slug(repo)}.git', str(checkout))
    run('git', 'config', 'user.name', OWNER, cwd=checkout)
    run('git', 'config', 'user.email', '3391146750@qq.com', cwd=checkout)
    return target, checkout


def claude(target, checkout, task):
    prompt = (ROOT / 'instructions.txt').read_text() + '\n\nTASK:\n' + task
    (target / 'prompt.txt').write_text(prompt)
    env = {key: value for key, value in os.environ.items() if key not in SCRUB}
    env['PATH'] = f"{ROOT / 'bin'}:{env.get('PATH', '/usr/bin')}"
    env['HYPR_MAINT_BIN'] = str(ROOT / 'bin')
    env['HYPR_MAINT_RUN'] = str(target)
    with (target / 'claude.json').open('w') as output:
        subprocess.run(['claude', '-p', '--model', 'opus', '--output-format', 'json',
                        '--permission-mode', 'acceptEdits', '--permission-prompts', 'none',
                        '--allowedTools', 'Bash,Read,Write,Edit,Glob,Grep,Agent', '--', prompt],
                       cwd=checkout, env=env, stdout=output, check=True, timeout=18000)
    result = json.loads((target / 'claude.json').read_text())
    log(f"agent run {target.name}: cost=${result.get('total_cost_usd') or 0:.2f} turns={result.get('num_turns')} error={result.get('is_error')}")
    if result.get('permission_denials'):
        # A denied call is a warning, not a failure: the receipt below is the authority on whether the item is handled.
        log('denied tool calls: ' + '; '.join(d.get('tool_input', {}).get('command', d.get('tool_name', '?'))[:120]
                                             for d in result['permission_denials']))
    if result.get('is_error'):
        log(result.get('result', ''))
        raise RuntimeError(f'Claude failed; inspect {target}')
    log(result.get('result', ''))
    receipt = json.loads((target / 'receipt.json').read_text())
    log(f'receipt {target.name}: {receipt}')
    if receipt.get('status') == 'deferred':
        # Handled until new activity, but the user has to act (for example a reporter asked for the maintainer personally).
        notify('Plugin item needs you', receipt.get('reason', ''), 'critical')
        return receipt
    if receipt.get('status') != 'completed':
        raise RuntimeError(f'Incomplete task; preserved for retry: {target}')
    return receipt


def update_local(repo, branch, published):
    """Fast-forward the user's local branch (wherever it is checked out) to what the agent pushed; never discard work."""
    local = Path(CONFIG['repos'][repo]['local'])
    if not (local / '.git').exists():
        return f'{repo}/{branch}: no local checkout'
    remote = next((name for name in run('git', 'remote', cwd=local).split()
                   if run('git', 'remote', 'get-url', name, cwd=local).rstrip('/').removesuffix('.git').lower().endswith(slug(repo).lower())), None)
    if not remote:
        return f'{repo}/{branch}: no remote for {slug(repo)}'
    run('git', 'fetch', '--quiet', remote, branch, cwd=local)
    if subprocess.run(['git', 'rev-parse', '--verify', '--quiet', f'refs/heads/{branch}'], cwd=local, capture_output=True).returncode != 0:
        return f'{repo}/{branch}: no local branch'
    current = run('git', 'rev-parse', f'refs/heads/{branch}', cwd=local)
    if current == published:
        return f'{repo}/{branch}: local already at {published[:9]}'
    if subprocess.run(['git', 'merge-base', '--is-ancestor', current, published], cwd=local).returncode != 0:
        return f'{repo}/{branch}: local {current[:9]} has commits not on {remote}/{branch}; left untouched'
    worktree = None
    entries = run('git', 'worktree', 'list', '--porcelain', cwd=local).split('\n\n')
    for entry in entries:
        lines = entry.splitlines()
        if f'branch refs/heads/{branch}' in lines:
            worktree = Path(lines[0].split(' ', 1)[1])
    if worktree is None:
        run('git', 'update-ref', f'refs/heads/{branch}', published, current, cwd=local)
        return f'{repo}/{branch}: fast-forwarded {current[:9]}..{published[:9]}'
    if run('git', 'status', '--porcelain', '--untracked-files=no', cwd=worktree):
        return f'{repo}/{branch}: checked out in {worktree} with uncommitted changes; left untouched'
    run('git', 'merge', '--ff-only', '--quiet', published, cwd=worktree)
    return f'{repo}/{branch}: fast-forwarded {current[:9]}..{published[:9]} in {worktree}'


def handle(repo, item, data):
    key = f'{repo}#{item["number"]}'
    target, checkout = workspace(f'{repo}-{item["number"]}', repo)
    (target / 'github-context.json').write_text(json.dumps(data, indent=2))
    meta = CONFIG['repos'][repo]
    kind = 'pull request' if 'pull_request' in item else 'issue'
    smoke = ' '.join(f'"{d}"' for d in meta.get('smoke', []))
    receipt = claude(target, checkout, f'''Handle {kind} https://github.com/{slug(repo)}/issues/{item["number"]} of {slug(repo)}.
This checkout is a fresh clone of the repository with all remote branches; the run directory is {target}
and the initial GitHub snapshot is in {target}/github-context.json. Re-read the item with gh before acting.
Repository notes: {meta.get('notes', 'none')}. Compositor plugin: {'yes' if meta.get('plugin') else 'no'}.
Smoke dispatchers for plugin-smoke.sh: {smoke or '(none: load/unload only)'}.
Hyprland target per branch (arguments for hyprland-prefix.sh, which prints the prefix directory):
{branch_targets(repo, checkout)}
Write {target}/receipt.json when done: {{"status":"completed","pushed":[{{"branch":"...","sha":"..."}}],"summary":"..."}}
listing every branch you pushed with its new head; {{"status":"deferred","reason":"..."}} when the item must wait for the
user personally (nothing posted); or {{"status":"blocked","reason":"..."}} after a transient failure (retried later).''')
    for push in receipt.get('pushed', []):
        try:
            log(update_local(repo, push['branch'], push['sha']))
        except (subprocess.CalledProcessError, KeyError) as error:
            log(f'{key}: local update failed: {error}')
    return key


def triage(check, only=None):
    statefile = STATE / 'handled.json'
    handled = json.loads(statefile.read_text()) if statefile.exists() else {}
    errors, seen = [], 0
    for repo in CONFIG['repos']:
        if only and repo not in only:
            continue
        for item in paginate(f'repos/{slug(repo)}/issues?state=open&per_page=100'):
            seen += 1
            key = f'{repo}#{item["number"]}'
            data = snapshot(repo, item)
            if handled.get(key) == digest(data):
                continue
            log(f'Needs handling: {key} {item["title"]}')
            if check:
                continue
            try:
                handle(repo, item, data)
                handled[key] = digest(snapshot(repo, item))
                notify(f'{key} handled', item['title'])
                temp = statefile.with_suffix('.tmp')
                temp.write_text(json.dumps(handled, indent=2))
                temp.replace(statefile)
            except Exception as error:
                errors.append(f'{key}: {error}')
                log(errors[-1])
    log(f'Checked {seen} open issues/PRs across {len(only or CONFIG["repos"])} repositories.')
    if errors:
        notify('Plugin triage needs attention', '; '.join(errors), 'critical')
        raise RuntimeError('; '.join(errors))


def default_branch(repo, checkout):
    return run('git', 'rev-parse', '--abbrev-ref', 'origin/HEAD', cwd=checkout).split('/', 1)[1]


def sync(check, only=None):
    """Merge each repository's default branch into its long-lived sibling branches (repos.json sync_into)."""
    errors = []
    for repo, meta in CONFIG['repos'].items():
        if (only and repo not in only) or not meta.get('sync_into'):
            continue
        target, checkout = workspace(f'{repo}-sync', repo)
        base = default_branch(repo, checkout)
        behind = [branch for branch in meta['sync_into']
                  if subprocess.run(['git', 'rev-parse', '--verify', '--quiet', f'origin/{branch}'], cwd=checkout, capture_output=True).returncode == 0
                  and subprocess.run(['git', 'merge-base', '--is-ancestor', f'origin/{base}', f'origin/{branch}'], cwd=checkout).returncode != 0]
        if not behind:
            log(f'{repo}: {base} already merged into {", ".join(meta["sync_into"])}')
            subprocess.run(['rm', '-rf', str(target)])
            continue
        log(f'{repo}: {base} not yet merged into {", ".join(behind)}')
        if check:
            continue
        smoke = ' '.join(f'"{d}"' for d in meta.get('smoke', []))
        try:
            receipt = claude(target, checkout, f'''Branch sync for {slug(repo)}: merge origin/{base} into {", ".join(behind)}.
This checkout is a fresh clone with all remote branches; the run directory is {target}.
Repository notes: {meta.get('notes', 'none')}. Compositor plugin: {'yes' if meta.get('plugin') else 'no'}.
Smoke dispatchers for plugin-smoke.sh: {smoke or '(none: load/unload only)'}.
Hyprland target per branch (arguments for hyprland-prefix.sh, which prints the prefix directory):
{branch_targets(repo, checkout)}
For each branch: merge {base} with a merge commit (no rebase, no squash), resolving conflicts so the branch keeps
its own Hyprland-version adaptations and Omarchy specifics while taking every {base} change. Then run the tier-2
gates for that branch (plugin-build.sh against the branch target, clang-format on files you had to edit by hand,
plugin-smoke.sh for compositor plugins, a read-only Codex review when you resolved conflicts by hand). Push the
branch only when its gates pass; a branch whose gates fail stays unpushed: preserve the merge on
auto/sync-<branch>, open a PR against <branch> describing the failure, and continue with the other branches.
Write {target}/receipt.json: {{"status":"completed","pushed":[{{"branch":"...","sha":"..."}}],"summary":"..."}} with
every branch you pushed, or {{"status":"blocked","reason":"..."}} after a transient failure.''')
            for push in receipt.get('pushed', []):
                log(update_local(repo, push['branch'], push['sha']))
        except Exception as error:
            errors.append(f'{repo} sync: {error}')
            log(errors[-1])
    if errors:
        notify('Plugin branch sync needs attention', '; '.join(errors), 'critical')
        raise RuntimeError('; '.join(errors))


def notifications(check):
    """Run triage for a repository as soon as GitHub reports activity there, instead of waiting for the timer."""
    active, threads = [], []
    for repo in CONFIG['repos']:
        found = paginate(f'repos/{slug(repo)}/notifications?all=false&per_page=50')
        if found:
            active.append(repo)
            threads += found
            for thread in found:
                log(f"  {repo}: {thread['reason']}: {thread['subject']['type']} {thread['subject']['title']}")
    log(f'{len(threads)} unread notification(s) in {len(active)} repositories.')
    if not threads or check:
        return
    try:
        triage(False, only=set(active))
    finally:
        for thread in threads:
            run('gh', 'api', '--method', 'PATCH', f"notifications/threads/{thread['id']}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['triage', 'notifications', 'sync'])
    parser.add_argument('--check', action='store_true', help='Read-only discovery; never launch agents')
    parser.add_argument('--repo', action='append', help='Limit triage to these repositories')
    args = parser.parse_args()
    MODE = args.mode + (' --check' if args.check else '')
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE / 'maintenance.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log('Another maintenance run is active; next timer will retry.')
            raise SystemExit(0)
        try:
            if args.mode == 'triage':
                triage(args.check, set(args.repo) if args.repo else None)
            elif args.mode == 'sync':
                sync(args.check, set(args.repo) if args.repo else None)
            else:
                notifications(args.check)
        except Exception as error:
            log(f'FAILED: {type(error).__name__}: {error}')
            raise
