# `physics`: movement like the vanilla client

`mcscon::physics` reproduces the vanilla 1.10 – 1.12.2 player movement closely enough that servers accept it without
"moved wrongly" corrections: gravity (0.08), drag (0.98), block friction, jumping (0.42), step-up (0.6), exact per-block
collision boxes (slabs, stairs, fences, …), water, lava and ladders. It runs on the [`World`](world.md) model. A
[`Session`](session.md) runs it for you every 50 ms from its `controls`; use these functions directly for custom
controllers or tests.

## `Controls`

```rust
pub struct Controls { pub forward: bool, pub back: bool, pub left: bool, pub right: bool,
                      pub jump: bool, pub sneak: bool, pub sprint: bool }
```

The "keys" you hold. `sprint` only has an effect together with `forward`; `sneak` is ignored while flying.
`Default` is all false.

## `Body`

```rust
pub struct Body { pub x: f64, pub y: f64, pub z: f64,          // feet position
                  pub vx: f64, pub vy: f64, pub vz: f64,       // blocks per tick
                  pub yaw: f32, pub pitch: f32,                // degrees
                  pub on_ground: bool, pub collided_h: bool, pub in_water: bool, pub in_lava: bool,
                  pub on_ladder: bool, pub jump_ticks: u8, pub sneaking: bool, pub sprinting: bool }
```

| Field | Meaning |
|-------|---------|
| `x, y, z` | position of the **feet** (the centre of the bounding box bottom) |
| `vx, vy, vz` | velocity in blocks per tick |
| `yaw`, `pitch` | view in degrees. Yaw 0 faces +Z (south), 90 faces −X (west), 180 −Z (north), 270 +X (east). Pitch −90 up … +90 down |
| `on_ground` | standing on something |
| `collided_h` | hit a wall in the last move |
| `in_water`, `in_lava`, `on_ladder` | environment |
| `sneaking`, `sprinting` | effective state (set by the tick from the controls) |

`height()` is 1.8 (1.65 sneaking), `eye_y()` is `y + 1.62` (`1.54` sneaking). The bounding box is 0.6 wide.

## Functions

```rust
pub fn physics_tick(b: &mut Body, input: &Controls, w: &World, flying: bool, walk_speed: f32, fly_speed: f32) -> bool
pub fn physics_move(b: &mut Body, dx: f64, dy: f64, dz: f64, w: &World)
```

- `physics_tick` advances **one 50 ms tick**. Returns `false` and does nothing if the chunk under the body is not
  loaded yet (movement before the world arrives would make the bot fall through the void).
  `flying`: creative flight (no gravity). `walk_speed` / `fly_speed`: the ability values (`0.1` and `0.05` by default;
  `PlayerState` has the server's values).
- `physics_move` moves with collision by `(dx, dy, dz)` in the order vanilla uses (Y, then X, then Z), including step-up.

```rust
let mut body = Body { x: 0.5, y: 5.0, z: 0.5, ..Default::default() };
let keys = Controls { forward: true, ..Default::default() };
for _ in 0..20 {                                      // one second
    if !physics_tick(&mut body, &keys, &world, false, 0.1, 0.05) { break; }
}
```

Determinism: the maths uses `libm`, so results are identical on every platform.
