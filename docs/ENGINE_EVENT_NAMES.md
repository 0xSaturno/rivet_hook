<!--
SPDX-FileCopyrightText: 2025-2026 Neptuwunium

SPDX-License-Identifier: EUPL-1.2
-->

# Engine event names and debug config

Reference only. Nothing in the hook reads these yet. They are event class names
and component names taken from two shipped config assets, kept here in case the
Lua bridge ever needs to subscribe to actor events.

Both assets were dumped with the DDL walker (`Type`/`Value` nesting). Names are
copied verbatim from the dumps, dated 2026-09-19. Whether the game honors edits
to either asset was **not tested**.

## `DebugActorSystemConfig`

Config for the dev-only actor system debug overlay. Two top level keys: a
`FilterList` of named event groups, and a `ThreeDViewWhiteList` of components.

Every event item is `{ Type: <event class name>, Obj: {} }`. The `Obj` is empty
in every entry, so no per-event parameters are known.

Shape quirk: a filter holding one event stores `EventList.Value` as a single
object, not an array (`Footsteps` does). A parser must accept both.

### Filters

| Filter | Events |
|---|---|
| Hero Common Spam | `StateUpdateCameraCallbackEvent`, `HeroTargetTrackerResolvedEvent` |
| Hero State Spam | `AllowEarlyTransitionEvent`, `AnimScaleTranslationOnEvent`, `AnimScaleTranslationOffEvent`, `JumpPeakEvent`, `MeleeComboBeginEvent`, `MeleeComboEndEvent`, `MeleeDamageBeginEvent`, `MeleeDamageEndEvent`, `MeleeStreakBeginEvent`, `MeleeStreakEndEvent` |
| Footsteps | `FootstepEvent` |
| SoundEvents | `SoundEvent`, `WWiseEvent` |
| Scripting | `CurveLoopedEvent`, `CommandDoneEvent`, `SpawnerStartedEvent`, `SpawnerStoppedEvent` |

The filter names read as "hide these, they flood the log", so these are the
high frequency events. Expect them to fire every frame or every step.

### `ThreeDViewWhiteList`

| Component | Notes |
|---|---|
| `SkirmishAwareness` | |
| `SkirmishJobHost` | |
| `Behavior` | `WhiteListChildren: true`, so child components are included |

## `DevstatsSystemConfig`

Telemetry config, not gameplay. `EventTypes` is a list of
`{ EventCodeName, EnabledConfigurations }`. The configurations are `kDebug`,
`kCoreOpt`, `kRelease` and `kFinal`.

Enabled in all four configurations:

`BootStartDevstatsEvent`, `UserProfileDevstatsEvent`,
`StartPlaythroughDevstatsEvent`, `StartMissionDevstatsEvent`,
`EndMissionDevstatsEvent`, `StartObjectiveDevstatsEvent`,
`EndObjectiveDevstatsEvent`, `DevstatsSystemMenuOptionChangedEvent`,
`GameStartDevstatsEvent`, `GameEndDevstatsEvent`,
`DevstatsAccessibilityUseShortcutEvent`, `PSNLinkDevstatsEvent`,
`AuthFailureDevstatsEvent`, `AuthSuccessDevstatsEvent`,
`HardwareProfileDevstatsEvent`

Enabled in none (empty `EnabledConfigurations`):

`DamageEvent`, `DamageDealtEvent`, `DevstatsEmergentVOFailedEvent`,
`DevstatsSegmentMappingEvent`

`DamageEvent` and `DamageDealtEvent` are the only two here that look like
gameplay events rather than telemetry wrappers. Whether they are the same class
the actor event system dispatches is **assumed, not checked**.

## Possible uses

- Candidate event names for a future `rivet.on_event(name, fn)` binding. The
  Scripting group (`SpawnerStartedEvent`, `SpawnerStoppedEvent`,
  `CommandDoneEvent`) and the melee events are the most script relevant.
- A check that the DDL walker handles the single-object-or-array quirk above.
- A starting point if a debug overlay switch is ever found. None is known.
