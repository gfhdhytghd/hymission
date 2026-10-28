#!/usr/bin/env python3
"""Check hook ABIs against headers and unique exported targets, without a compositor.

Usage: python tools/verify_dev_hooks.py --build-dir build-port --hyprland /path/to/Hyprland
       python tools/verify_dev_hooks.py --build-dir build-port --types-only   (CTest: headers only)
The Hyprland binary is inspected with nm; it is never executed.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]

# Method types are checked using the actual aliases compiled into the plugin.
OVERVIEW = {
    'SurfaceGetTexBoxFn': ('CSurfacePassElement', 'getTexBox'),
    'SurfaceBoundingBoxFn': ('CSurfacePassElement', 'boundingBox'),
    'SurfaceOpaqueRegionFn': ('CSurfacePassElement', 'opaqueRegion'),
    'SurfaceVisibleRegionFn': ('CSurfacePassElement', 'visibleRegion'),
    'SurfaceDrawFn': ('IPassElement', 'draw'),
    'SurfaceBlurNeedsFn': ('CSurfacePassElement', 'needsLiveBlur'),
    'ShouldRenderWindowFn': ('Render::IHyprRenderer', 'shouldRenderWindow'),
    'EffectiveAlphaFn': ('Desktop::View::CWindowPresentation', 'renderPresentation'),
    'RendererDrawElementFn': ('Render::IHyprRenderer', 'draw'),
    'RenderLayerFn': ('Render::IHyprRenderer', 'renderLayer'),
    'CustomShadowFn': ('Render::GL::CHyprGLRenderer', 'drawShadow'),
    'BorderDrawFn': ('CHyprBorderDecoration', 'draw'),
    'CalculateUVForSurfaceFn': ('Render::IElementRenderer', 'calculateUVForSurface'),
    'WorkspaceSwipeBeginFn': ('CWorkspaceSwipeGesture', 'begin'),
    'WorkspaceSwipeUpdateFn': ('CWorkspaceSwipeGesture', 'update'),
    'WorkspaceSwipeEndFn': ('CWorkspaceSwipeGesture', 'end'),
    'UnifiedWorkspaceSwipeBeginFn': ('CUnifiedWorkspaceSwipeGesture', 'begin'),
    'UnifiedWorkspaceSwipeUpdateFn': ('CUnifiedWorkspaceSwipeGesture', 'update'),
    'UnifiedWorkspaceSwipeEndFn': ('CUnifiedWorkspaceSwipeGesture', 'end'),
    'ScrollMoveGestureBeginFn': ('CScrollMoveTrackpadGesture', 'begin'),
    'ScrollMoveGestureUpdateFn': ('CScrollMoveTrackpadGesture', 'update'),
    'ScrollMoveGestureEndFn': ('CScrollMoveTrackpadGesture', 'end'),
}
STAGE = {
    'RecheckFn': ('Layout::CSpace', 'recheckWorkArea'),
    'DragEndFn': ('Layout::Supplementary::CDragStateController', 'dragEnd'),
    'WindowFocusFn': ('Desktop::CFocusState', 'rawWindowFocus'),
    'ScrollFocusFn': ('Layout::Tiled::CScrollingAlgorithm', 'focusOnInput'),
    'WindowAtFn': ('Desktop::CViewHitTester', 'windowAt'),
    'RoundingFn': ('Desktop::View::CWindowPresentation', 'rounding'),
    'RenderWindowFn': ('Render::IHyprRenderer', 'renderWindow'),
    'ShouldBlurFn': ('Desktop::View::CWindow', 'shouldBlur'),
    'SurfaceBoxFn': ('CSurfacePassElement', 'getTexBox'),
    'SurfaceVisibleFn': ('CSurfacePassElement', 'visibleRegion'),
    'SurfaceUVFn': ('Render::IElementRenderer', 'calculateUVForSurface'),
    'ChangeWorkspaceFn': ('Monitor::CMonitor', 'changeWorkspace'),
    'SwipeBeginFn': ('CUnifiedWorkspaceSwipeGesture', 'begin'),
    'SwipeUpdateFn': ('CUnifiedWorkspaceSwipeGesture', 'update'),
    'SwipeEndFn': ('CUnifiedWorkspaceSwipeGesture', 'end'),
    'BlurFramebufferFn': ('Render::IHyprRenderer', 'blurMainFramebuffer'),
    'FadeoutsFn': ('Render::IHyprRenderer', 'renderFadeouts'),
}


def aliases(text):
    return dict(re.findall(r'using\s+(\w+Fn)\s*=\s*([^;]+);', text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--hyprland', type=Path)
    parser.add_argument('--types-only', action='store_true', help='check hook types against the configured headers; skip the binary symbol audit')
    args = parser.parse_args()
    if not args.types_only and not args.hyprland:
        parser.error('--hyprland is required unless --types-only is given')
    build = args.build_dir.resolve()
    overview = (ROOT / 'src/overview_controller.cpp').read_text()
    stage = (ROOT / 'src/stage_controller.cpp').read_text()
    action = (ROOT / 'src/action_hooks.hpp').read_text()
    oa = aliases((ROOT / 'src/overview_controller.hpp').read_text())
    sa = aliases(stage)
    aa = aliases(action)
    generated = r'''
#include <bits/stdc++.h>
// Read private declarations only in this offline type check. Production uses
// function hooks, and never accesses these renderer members directly.
#define private public
#define protected public
#include "overview_controller.hpp"
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ScrollMoveGesture.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/render/GLRenderer.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#undef protected
#undef private
using namespace hymission;
template<class Fn, class Class> struct Member;
template<class R, class This, class... Args, class Class>
struct Member<R (*)(This, Args...), Class> {
    using Type = std::conditional_t<std::is_const_v<std::remove_pointer_t<This>>,
        R (Class::*)(Args...) const, R (Class::*)(Args...)>;
    static_assert(std::is_convertible_v<std::conditional_t<std::is_const_v<std::remove_pointer_t<This>>, const Class*, Class*>, This>);
};
'''
    count = 0
    for ns, mapping, types in [('Overview', OVERVIEW, oa), ('Stage', STAGE, sa)]:
        generated += f'namespace {ns} {{\n'
        for name, (cls, method) in mapping.items():
            generated += f'using {name} = {types[name]};\n'
            generated += f'static_assert(std::is_member_function_pointer_v<decltype(static_cast<Member<{name}, {cls}>::Type>(&{cls}::{method}))>);\n'
            count += 1
        generated += '}\n'
    generated += f'using AddPassFn = {sa["AddPassFn"]};\n'
    generated += 'static_assert(std::is_same_v<AddPassFn, decltype(&Render::IHyprRenderer::addPassElement)>);\n'
    generated += f'using WorkspaceAnimationFn = {sa["WorkspaceAnimationFn"]};\n'
    generated += 'static_assert(std::is_same_v<WorkspaceAnimationFn, decltype(&Animation::Workspace::startAnimation)>);\n'
    generated += 'static_assert(std::is_same_v<Overview::BorderDrawFn, void (*)(void*, Render::CRenderContext&, PHLMONITOR, const float&, const Render::SWindowRenderPresentation&)>);\n'
    for cls in ['CHyprDropShadowDecoration', 'CHyprGroupBarDecoration']:
        generated += f'static_assert(std::is_member_function_pointer_v<decltype(static_cast<Member<Overview::BorderDrawFn, {cls}>::Type>(&{cls}::draw))>);\n'
    generated += 'static_assert(std::is_member_function_pointer_v<decltype(static_cast<Member<Overview::SurfaceBlurNeedsFn, CSurfacePassElement>::Type>(&CSurfacePassElement::needsPrecomputeBlur))>);\n'
    for name, method in [('FullscreenFn', 'fullscreenWindow'), ('FullscreenStateFn', 'fullscreenWindow'), ('WorkspaceFn', 'changeWorkspace'), ('WorkspaceFn', 'changeWorkspaceOnCurrentMonitor')]:
        generated += f'using Action{name} = {aa[name].replace("Result", "Config::Actions::ActionResult")};\n' if name != 'WorkspaceFn' or method == 'changeWorkspace' else ''
        generated += f'static_assert(std::is_same_v<Action{name}, decltype(static_cast<Action{name}>(&Config::Actions::{method}))>);\n'
    generated += 'using GradientFn = CAnimatedDecorationGradient::SRenderState (CAnimatedDecorationGradient::*)() const;\nstatic_assert(std::is_same_v<GradientFn, decltype(&CAnimatedDecorationGradient::renderState)>);\n'
    source = build / 'hook-abi-check.cpp'
    source.write_text(generated)
    flags_file = build / 'CMakeFiles/hymission.dir/flags.make'
    flags = dict(re.findall(r'^(CXX_\w+) = (.*)$', flags_file.read_text(), re.M))
    compiler = re.search(r'^CMAKE_CXX_COMPILER:[A-Z]+=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M)[1]
    cmd = [compiler] + shlex.split(' '.join(flags[key] for key in ['CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS']))
    cmd += ['-I', str(ROOT / 'src'), '-fsyntax-only', str(source)]
    subprocess.run(cmd, check=True)
    print(f'PASS: {count + 10} upstream hook/call signature checks (production also asserts thunk types)')
    if args.types_only:
        return

    raw = subprocess.check_output(['nm', '-D', '-j', '--defined-only', str(args.hyprland)], text=True).splitlines()
    demangled = subprocess.check_output(['c++filt'], input='\n'.join(raw), text=True).splitlines()
    symbols = list(zip(raw, demangled))
    audit = []
    pattern = r'(?:find|findFunction|hookFunction|attach)\("([^"\n]+)",\s*(?:"([^"\n]+)"|std::vector<std::string>\{([^}]+)\})'
    for filename, source_text in [('stage_controller.cpp', stage), ('overview_controller.cpp', overview), ('action_hooks.hpp', action)]:
        for match in re.finditer(pattern, source_text):
            needles = [match[2]] if match[2] else re.findall(r'"([^"]+)"', match[3])
            selected = []
            for needle in needles:
                selected = [(raw, demangled) for raw, demangled in symbols if match[1] in raw and (demangled == needle if filename == 'action_hooks.hpp' else needle in demangled)]
                if selected:
                    break
            if len(selected) != 1:
                raise RuntimeError(f'{filename}: {match[1]} {needles}: expected one target, found {len(selected)}')
            audit.append({'file': filename, 'search': match[1], 'filter': needle, 'mangled': selected[0][0], 'signature': selected[0][1], 'count': 1})
    (build / 'hook-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    print(f'PASS: {len(audit)} lookups, {len(set(row["mangled"] for row in audit))} unique exported targets; each resolves exactly once')
    # Tiles attaches only to an exact demangled signature and disables itself
    # otherwise; check every one against the binary so a native signature
    # change is found before it silently turns tiles off.
    exact = {}
    for _, name in symbols:
        exact[name] = exact.get(name, 0) + 1
    tiles = re.findall(r'return "([^"]+)";', (ROOT / 'src/tiles_symbols.hpp').read_text())
    missing = [sig for sig in tiles if exact.get(sig, 0) != 1]
    if missing:
        raise RuntimeError('tiles_symbols.hpp: expected exactly one exported target for:\n  ' + '\n  '.join(missing))
    print(f'PASS: {len(tiles)} exact tiles symbols resolve exactly once')


if __name__ == '__main__':
    main()
