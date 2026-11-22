# Mouse gestures

erssi lets you move between windows by dragging the mouse across the chat
area — a short "swipe" instead of a keyboard shortcut. Gestures are on by
default and work only in the main chat area, never over the side panels
(window list / nick list), so they don't fire when you click those.

## Using them

1. Press and hold the **left** mouse button in the chat area.
2. Drag **horizontally**, left or right.
3. Release to run the action. A short drag and a longer drag of the same
   direction run different actions.

## Default gestures

| Direction | Short drag | Long drag |
|-----------|------------|-----------|
| **Left**  | `/window prev` — previous window | `/window 1` — jump to window 1 (the network/status window) |
| **Right** | `/window next` — next window | `/window lastone` — the window you were in before this one |

Left for "back", right for "forward"; a short drag steps one window, a long
drag jumps.

## Settings

Change these with `/set <name> <value>`; `/set <name>` shows the current one.

| Setting | Default | What it does |
|---------|---------|--------------|
| `mouse_gestures` | `on` | Turn gesture recognition on or off. |
| `gesture_left_short` | `/window prev` | Command for a short left drag. |
| `gesture_left_long` | `/window 1` | Command for a long left drag. |
| `gesture_right_short` | `/window next` | Command for a short right drag. |
| `gesture_right_long` | `/window lastone` | Command for a long right drag. |
| `gesture_sensitivity` | `10` | Minimum drag distance, in terminal cells, before a drag counts as a gesture. |
| `gesture_timeout` | `1000` | Longest a drag may take, in milliseconds; a slower drag is ignored. |

Mouse-wheel scrolling of the chat area is a separate, related setting,
`mouse_scroll_chat` (also `on` by default); gestures and scrolling work
together.

To turn gestures off but keep wheel scrolling:

```
/set mouse_gestures off
```

## How a drag is recognised

A drag becomes a gesture only when all of this holds:

- it moves at least `gesture_sensitivity` cells;
- it is **more horizontal than vertical** (so accidental vertical drift while
  selecting text does not trigger it);
- it finishes within `gesture_timeout` milliseconds.

A drag longer than twice `gesture_sensitivity` is a *long* drag; a shorter one
that still clears the threshold is a *short* drag. Lower `gesture_sensitivity`
to trigger on smaller movements, raise it if you get accidental gestures.

## Customising

Any gesture can run any erssi command, so the four above are only defaults.

```
# jump to wherever activity is, on a long right drag
/set gesture_right_long "/window goto active"

# make a long left drag go to the previous window, like the short one
/set gesture_left_long "/window prev"

# run several commands at once, separated by ';'
/set gesture_right_long "/window lastone ; /scrollback end"

# run a Perl script
/set gesture_left_long "/script exec my_script.pl"
```

Put the most frequent action on a short drag (it is the easier movement) and
keep long drags for jumps. `/save` writes your choices to the configuration.

## Troubleshooting

- **Nothing happens.** Check `mouse_gestures` is `on`, and that you are
  dragging in the chat area rather than over a side panel.
- **Gestures fire by accident.** Raise `gesture_sensitivity` (e.g. `15`).
- **A gesture is missed.** Drag more horizontally, a little further, and
  finish within the timeout; lowering `gesture_sensitivity` also helps.

## For developers

The gesture code is in `src/fe-ansi/gui-gestures.c`. Settings are registered
in `gui_gestures_init()` (under the `fe-text`/`lookandfeel` module) and reread
on `setup changed`; a recognised gesture runs its command by emitting
`send command`. `gui_gestures_init()` runs before `sidepanels_init()` in
`textui_finish_init()` so the gesture layer can claim chat-area drags before
the side panels see them. See [ARCHITECTURE.md](../ARCHITECTURE.md) for how
the terminal frontend is put together.
