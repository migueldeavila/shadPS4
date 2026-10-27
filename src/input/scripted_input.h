// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Input::Scripted {

// Starts a thread that drives controller 0 from command scripts dropped into the log directory,
// so the game can be played without a person at the controller.
//
// Write a script to `<log dir>/input` (write `input.tmp`, then rename it). The thread runs it,
// deletes it and writes `input_done` ("<flips at start> <flips at end> <errors>"). Commands, one
// per line, `#` starts a comment:
//   press <buttons> [frames]  press, hold for `frames` flips (default 3, at least 100 ms), release
//   hold <buttons>            press and keep pressed
//   release <buttons>         release
//   axis <axis> <0-255>       set an axis and keep it (lx ly rx ry: 128 = centre; l2 r2: 0 = off)
//   stick <l|r> <x> <y> <ms>  set a stick for `ms`, then centre it
//   wait <ms>                 sleep
//   waitf <frames>            wait until `frames` more flips happened (10 s timeout)
//   shot                      save a screenshot of the next frame to the screenshots directory
//   reset                     release every button and centre every axis
// Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options touchpad,
// combined with `+` (e.g. `l1+r1`).
void StartScriptedInput();

} // namespace Input::Scripted
