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
FEISHU_USER = 'ou_3db23cf05c03b4942c355797cc88e8f9'
REPORT = {'done': [], 'attention': []}
REPORT_NAME = '插件运维'
RUN_STAMP = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
# Appended to every agent task: the receipt feeds the Feishu report, so it must be written for the user.
RECEIPT_RULES = '''The receipt summary must be Chinese, user-facing, at most 300 characters, and describe actions and evidence.
It is sent verbatim to the user's Feishu DM. Reasons must also be Chinese.
Add optional "attention":"..." in Chinese only when the user personally must act: a tier-3 PR awaits their
decision, an auto/* PR was opened, a contributor asks for the maintainer personally, or a manual step the
script is not allowed to perform (release, tag, marketplace submission, merging its own tier-3 PR).
Otherwise omit attention.
Set "idle": true when you posted, pushed, merged and closed nothing (for example no new activity since your last reply); such runs are not reported to the user.'''
# Nothing started from here may find the live compositor: the timer inherits the session's variables.
SCRUB = ('WAYLAND_DISPLAY', 'DISPLAY', 'HYPRLAND_INSTANCE_SIGNATURE', 'SWAYSOCK', 'HYPRLAND_CMD', 'XDG_SESSION_ID', 'LD_PRELOAD')


class Transient(Exception):
    """Temporary service failure; leave work unhandled for the next timer."""


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


def feishu(text, urgent=False):
    """Direct message to the user through lark-cli's bot identity. Best effort: never raises."""
    try:
        text = text[:2999] + '…' if len(text) > 3000 else text
        if os.environ.get('HYPR_MAINT_FEISHU') == '0':
            log(f'feishu: 仅记录（加急={urgent}）\n{text}')
            return
        key = f'{RUN_STAMP}-{MODE[:16]}-{"urgent" if urgent else "done"}'
        result = subprocess.run(['lark-cli', 'im', '+messages-send', '--as', 'bot', '--user-id', FEISHU_USER,
                                 '--text', text, '--idempotency-key', key],
                                capture_output=True, text=True, timeout=60, cwd=STATE)
        if result.returncode != 0:
            raise RuntimeError(f'发送失败：{result.returncode} {result.stderr}')
        payload = json.loads(result.stdout)
        if payload.get('ok') is not True:
            raise RuntimeError(f'发送失败：{result.stdout}')
        message_id = payload['data']['message_id']
        if not message_id:
            raise RuntimeError('发送结果缺少 message_id')
        if urgent:
            result = subprocess.run(['lark-cli', 'im', 'messages', 'urgent_app', '--as', 'bot',
                                     '--message-id', message_id, '--user-id-type', 'open_id',
                                     '--data', json.dumps({'user_id_list': [FEISHU_USER]})],
                                    capture_output=True, text=True, timeout=60, cwd=STATE)
            if result.returncode != 0 or json.loads(result.stdout).get('ok') is not True:
                raise RuntimeError(f'加急失败：{result.returncode} {result.stderr} {result.stdout}')
    except Exception as error:
        log(f'feishu: {type(error).__name__}: {error}')


def report():
    heading = f'[{REPORT_NAME} {MODE}]'
    if REPORT['done']:
        stamp = datetime.datetime.now().astimezone().strftime('%m-%d %H:%M %Z')
        feishu(f'{heading} {stamp}\n已完成 {len(REPORT["done"])} 项：\n\n' + '\n\n'.join(REPORT['done']))
    if REPORT['attention']:
        feishu(f'{heading} 需要你处理 {len(REPORT["attention"])} 项：\n\n' + '\n\n'.join(REPORT['attention']), urgent=True)


def report_attention(key, error, target=None):
    text = f'{key}：{error}'
    REPORT['attention'].append(text + (f'\n记录：{target}' if target else ''))


def report_done(key, title, receipt, target):
    if receipt.get('idle'):
        log(f'idle: {key}')
        return
    text = f'{key} {title}\n{receipt.get("summary", "")}'
    for push in receipt.get('pushed', []):
        text += f'\n推送：{push["branch"]}@{push["sha"][:9]}'
    REPORT['done'].append(text + f'\n记录：{target}')


def agent(key, title, target, checkout, task):
    try:
        receipt = claude(target, checkout, task)
        if receipt.get('attention'):
            report_attention(f'{key} {title}', receipt['attention'], target)
        if receipt.get('status') == 'deferred':
            report_attention(f'{key} {title}', receipt.get('reason', '等待用户本人处理'), target)
        elif receipt.get('status') != 'completed':
            raise RuntimeError(receipt.get('reason') or '任务未完成，保留记录以便重试')
        return receipt
    except Transient:
        raise
    except Exception as error:
        report_attention(key, f'代理运行失败（{type(error).__name__}）：{error}', target)
        raise


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=None).strip()


def api(endpoint, *options):
    command = ('gh', 'api', *options, endpoint)
    try:
        output = run(*command)
    except subprocess.CalledProcessError as error:
        raise Transient(f'{" ".join(command)}: {error}') from error
    return json.loads(output) if output else None


def paginate(endpoint):
    pages = api(endpoint, '--paginate', '--slurp')
    return [item for page in pages for item in page]


def pick(value, *fields):
    return {field: (value or {}).get(field) for field in fields}


def comment_snapshot(comment, *fields):
    return dict(pick(comment, 'id', 'body', *fields), user=pick(comment.get('user'), 'login'))


def slug(repo):
    return f'{OWNER}/{repo}'


def snapshot(repo, item):
    number = item['number']
    path = f'repos/{slug(repo)}'
    issue = api(f'{path}/issues/{number}')
    result = {'issue': dict(pick(issue, 'number', 'title', 'body', 'state', 'state_reason', 'locked'),
                            labels=None if issue.get('labels') is None else [pick(label, 'name') for label in issue['labels']],
                            assignees=None if issue.get('assignees') is None else [pick(user, 'login') for user in issue['assignees']],
                            milestone=pick(issue.get('milestone'), 'title')),
              'comments': [comment_snapshot(comment) for comment in paginate(f'{path}/issues/{number}/comments?per_page=100')]}
    if 'pull_request' in item:
        pull = api(f'{path}/pulls/{number}')
        result['pull'] = dict(pick(pull, 'draft', 'state', 'merged', 'merge_commit_sha'),
                              head=pick(pull.get('head'), 'sha'), base=pick(pull.get('base'), 'ref'))
        result['reviews_pr'] = [comment_snapshot(review, 'state', 'commit_id')
                                for review in paginate(f'{path}/pulls/{number}/reviews?per_page=100')]
        result['comments_pr'] = [comment_snapshot(comment, 'path', 'line', 'commit_id')
                                 for comment in paginate(f'{path}/pulls/{number}/comments?per_page=100')]
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
        process = subprocess.run(['claude', '-p', '--model', 'opus', '--output-format', 'json',
                        '--permission-mode', 'acceptEdits', '--permission-prompts', 'none',
                        '--allowedTools', 'Bash,Read,Write,Edit,Glob,Grep,Agent', '--', prompt],
                       cwd=checkout, env=env, stdout=output, stderr=subprocess.PIPE, text=True, check=False, timeout=18000)
    failure = f'Claude exit={process.returncode}; stderr tail: {(process.stderr or "")[-1000:]}; 记录：{target}'
    try:
        result = json.loads((target / 'claude.json').read_text())
        if not isinstance(result, dict):
            raise ValueError('Claude JSON is not an object')
    except (OSError, ValueError) as error:
        raise Transient(f'{failure}; invalid claude.json: {error}') from error
    status = result.get('api_error_status')
    message = str(result.get('result') or '')
    if (process.returncode != 0 or result.get('api_error') or status == 429
            or (isinstance(status, int) and status >= 500)
            or 'usage_limit' in message.lower() or 'weekly limit' in message.lower()):
        raise Transient(f'{failure}; api_error={result.get("api_error")} status={status}; {message}')
    log(f"agent run {target.name}: cost=${result.get('total_cost_usd') or 0:.2f} turns={result.get('num_turns')} error={result.get('is_error')}")
    if result.get('permission_denials'):
        # A denied call is a warning, not a failure: the receipt below is the authority on whether the item is handled.
        log('denied tool calls: ' + '; '.join(d.get('tool_input', {}).get('command', d.get('tool_name', '?'))
                                             for d in result['permission_denials']).replace('\n', ' ')[:120])
    if result.get('is_error'):
        log(result.get('result', ''))
        raise RuntimeError(f'Claude 运行失败；记录：{target}')
    log(result.get('result', ''))
    receipt = json.loads((target / 'receipt.json').read_text())
    log(f'receipt {target.name}: {receipt}')
    if receipt.get('status') == 'deferred':
        # Handled until new activity, but the user has to act (for example a reporter asked for the maintainer personally).
        notify('Plugin item needs you', receipt.get('reason', ''), 'critical')
        return receipt
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
        if f'branch refs/heads/{branch}' in lines and 'prunable' not in entry:
            candidate = Path(lines[0].split(' ', 1)[1])
            if candidate.is_dir():  # a deleted worktree still lists the branch until it is pruned
                worktree = candidate
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
    receipt = agent(key, item['title'], target, checkout, f'''Handle {kind} https://github.com/{slug(repo)}/issues/{item["number"]} of {slug(repo)}.
This checkout is a fresh clone of the repository with all remote branches; the run directory is {target}
and the initial GitHub snapshot is in {target}/github-context.json. Re-read the item with gh before acting.
Repository notes: {meta.get('notes', 'none')}. Compositor plugin: {'yes' if meta.get('plugin') else 'no'}.
Smoke dispatchers for plugin-smoke.sh: {smoke or '(none: load/unload only)'}.
Hyprland target per branch (arguments for hyprland-prefix.sh, which prints the prefix directory):
{branch_targets(repo, checkout)}
Write {target}/receipt.json when done: {{"status":"completed","pushed":[{{"branch":"...","sha":"..."}}],"summary":"..."}}
listing every branch you pushed with its new head; {{"status":"deferred","reason":"..."}} when the item must wait for the
user personally (nothing posted); or {{"status":"blocked","reason":"..."}} after a transient failure (retried later).
{RECEIPT_RULES}''')
    if receipt.get('status') == 'completed':
        report_done(key, item['title'], receipt, target)
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
            try:
                data = snapshot(repo, item)
                if handled.get(key) == digest(data):
                    continue
                log(f'Needs handling: {key} {item["title"]}')
                if check:
                    continue
                handle(repo, item, data)
                handled[key] = digest(snapshot(repo, item))
                notify(f'{key} handled', item['title'])
                temp = statefile.with_suffix('.tmp')
                temp.write_text(json.dumps(handled, indent=2))
                temp.replace(statefile)
            except Transient as error:
                log(f'{key}: transient: {error}')
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
            receipt = agent(repo, '分支同步', target, checkout, f'''Branch sync for {slug(repo)}: merge origin/{base} into {", ".join(behind)}.
This checkout is a fresh clone with all remote branches; the run directory is {target}.
Repository notes: {meta.get('notes', 'none')}. Compositor plugin: {'yes' if meta.get('plugin') else 'no'}.
Smoke dispatchers for plugin-smoke.sh: {smoke or '(none: load/unload only)'}.
Hyprland target per branch (arguments for hyprland-prefix.sh, which prints the prefix directory):
{branch_targets(repo, checkout)}
For each branch: merge {base} with a merge commit (no rebase, no squash), resolving conflicts so the branch keeps
its own Hyprland-version adaptations and Omarchy specifics while taking every {base} change. Then run the tier-2
gates for that branch (plugin-build.sh --trusted against the branch target, clang-format on files you had to edit
by hand, plugin-smoke.sh for compositor plugins, a read-only Codex review when you resolved conflicts by hand).
A test that also fails on the unmerged branch in the same mode is pre-existing: name it, do not count it. Push the
branch only when its gates pass; a branch whose gates fail stays unpushed: preserve the merge on
auto/sync-<branch>, open a PR against <branch> describing the failure, and continue with the other branches.
Write {target}/receipt.json: {{"status":"completed","pushed":[{{"branch":"...","sha":"..."}}],"summary":"..."}} with
every branch you pushed, or {{"status":"blocked","reason":"..."}} after a transient failure.
{RECEIPT_RULES}''')
            if receipt.get('status') == 'completed':
                report_done(repo, '分支同步', receipt, target)
            for push in receipt.get('pushed', []):
                log(update_local(repo, push['branch'], push['sha']))
        except Transient as error:
            log(f'{repo}: transient: {error}')
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
            api(f"notifications/threads/{thread['id']}", '--method', 'PATCH')


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
        except Transient as error:
            log(f'TRANSIENT: {error}')
        except Exception as error:
            log(f'FAILED: {type(error).__name__}: {error}')
            if not REPORT['attention']:  # item failures are already listed
                report_attention(MODE + ' 运行失败', str(error))
            raise
        finally:
            report()
