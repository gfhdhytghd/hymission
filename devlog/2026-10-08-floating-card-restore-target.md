# Smooth floating-window restore to the native target

Ordinary workspace-switch flights still used `transitionBoxWithin` for incoming
floating windows. That function fits the desktop endpoint into the output, so
the final Stage frame differed from the real window's position/size. Clearing
the flight revealed the real endpoint as a jump from the edge.

Mark incoming floating workspace flights as having a native destination. Fit
only a fresh sidebar source frame, preserving the previous sidebar constraint,
then interpolate directly to the real desktop endpoint. Rebuilt/interrupted
flights retain their sampled source, including offscreen coordinates. Outgoing
sidebar constraints and scrolling viewport flight geometry are unchanged.

Validation covers native destinations beyond either output edge and an oversized
destination on a negative-coordinate monitor: exact final geometry, convergence
just before handoff, and no source refitting on restart. CMake build and CTest
are required; live restore acceptance remains pending user-run `hyprpm update`.
No compositor reload was performed.
