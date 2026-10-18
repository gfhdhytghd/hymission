# Stage integrated workspace transitions (FB10)

## Single-column regression boundary

The task starts at `5df9917`. The loadable pretiles development reference is `552bd339fe4809e0a031d277ca97f4dac6d0a5da`; its archived source matches `build-tiles/reference-src` byte-for-byte for Stage. `origin/master` retains the same Stage flight and swipe design, but targets the older window/workspace ABI (`desktop/view/Window.hpp`, `m_id`, `m_isFloating`). It is compared as source, not loaded into the e5a03b6 compositor as if ABI-compatible.

The history boundary for the single-tile card regression is `b38136c60d4f3c2f41a7bc97d833976f15c3210e`, which introduced `nativeCardActivation`. That flag explicitly bypasses Stage workspace-animation ownership and excludes the synchronous pending-flight preparation when a card is clicked with one enabled tile. The request to retain the native `changeWorkspace` call was implemented as a request to use its native slide, losing Stage's card-to-desktop flight. Keeping the native workspace action does not require giving it visual ownership.

Both boundary revisions were independently archived, compiled against the matching release headers, and tested in private compositors: `b38136c^` passes the card flight/progress checks; `b38136c` fails both. The current pre-fix binary also fails only the card route in the single-tile case. Removing this flag restores all key, relative, card and four-finger routes in the single-tile case. Native `changeWorkspace` still commits the switch.

The reported tiles-off animation loss is **not reproduced** in the controlled dwindle or scrolling cases: the current pre-fix binary and pretiles reference both produce Stage flights for keys, relative selectors, cards and swipes. No separate tiles-off cause is claimed. The off-mode reference remains a required control for the multitile changes.

Private artifacts: `build-tiles/fb10-reference`, `fb10-before-{off,single,two}`, `fb10-before-off-scrolling`, `fb10-bisect-{good,bad}`, and `fb10-single-fixed`. Each route records Stage state, every-rendered-frame native framebuffer readback, and native window/workspace state. These establish private compositor behavior, not physical device or host-session acceptance. No push or host reload is performed.

## Multitile implementation and final validation

To be completed by the second implementation phase.
