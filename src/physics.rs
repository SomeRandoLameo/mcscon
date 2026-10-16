//! Player movement physics replicating vanilla 1.10-1.12.2 (gravity, drag, friction, exact per-state AABB
//! collision, step-up, water, ladders).
use crate::blocks;
use crate::world::World;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Controls {
    pub forward: bool,
    pub back: bool,
    pub left: bool,
    pub right: bool,
    pub jump: bool,
    pub sneak: bool,
    pub sprint: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Body {
    /// Feet position.
    pub x: f64,
    pub y: f64,
    pub z: f64,
    /// Blocks per tick.
    pub vx: f64,
    pub vy: f64,
    pub vz: f64,
    /// Degrees; yaw 0 = +Z (south), 90 = -X (west).
    pub yaw: f32,
    pub pitch: f32,
    pub on_ground: bool,
    pub collided_h: bool,
    pub in_water: bool,
    pub in_lava: bool,
    pub on_ladder: bool,
    pub jump_ticks: u8,
    pub sneaking: bool,
    pub sprinting: bool,
}
impl Body {
    pub fn height(&self) -> f64 {
        if self.sneaking {
            1.65
        } else {
            1.8
        }
    }
    pub fn eye_y(&self) -> f64 {
        self.y + if self.sneaking { 1.54 } else { 1.62 }
    }
}

#[derive(Clone, Copy)]
struct Box {
    x0: f64,
    y0: f64,
    z0: f64,
    x1: f64,
    y1: f64,
    z1: f64,
}

const HALF: f64 = 0.3;
const STEP: f64 = 0.6;

fn body_box(b: &Body) -> Box {
    Box { x0: b.x - HALF, y0: b.y, z0: b.z - HALF, x1: b.x + HALF, y1: b.y + b.height(), z1: b.z + HALF }
}

fn clip_x(b: &Box, m: &Box, mut d: f64) -> f64 {
    if m.y1 <= b.y0 || m.y0 >= b.y1 || m.z1 <= b.z0 || m.z0 >= b.z1 {
        return d;
    }
    if d > 0.0 && m.x1 <= b.x0 {
        let c = b.x0 - m.x1;
        if c < d {
            d = c;
        }
    } else if d < 0.0 && m.x0 >= b.x1 {
        let c = b.x1 - m.x0;
        if c > d {
            d = c;
        }
    }
    d
}
fn clip_y(b: &Box, m: &Box, mut d: f64) -> f64 {
    if m.x1 <= b.x0 || m.x0 >= b.x1 || m.z1 <= b.z0 || m.z0 >= b.z1 {
        return d;
    }
    if d > 0.0 && m.y1 <= b.y0 {
        let c = b.y0 - m.y1;
        if c < d {
            d = c;
        }
    } else if d < 0.0 && m.y0 >= b.y1 {
        let c = b.y1 - m.y0;
        if c > d {
            d = c;
        }
    }
    d
}
fn clip_z(b: &Box, m: &Box, mut d: f64) -> f64 {
    if m.x1 <= b.x0 || m.x0 >= b.x1 || m.y1 <= b.y0 || m.y0 >= b.y1 {
        return d;
    }
    if d > 0.0 && m.z1 <= b.z0 {
        let c = b.z0 - m.z1;
        if c < d {
            d = c;
        }
    } else if d < 0.0 && m.z0 >= b.z1 {
        let c = b.z1 - m.z0;
        if c > d {
            d = c;
        }
    }
    d
}

struct Clipped {
    dx: f64,
    dy: f64,
    dz: f64,
    bx: Box,
}

/// Clip motion against all solid blocks around the (motion-expanded) box. Order matches vanilla: Y, then X/Z.
fn clip_motion(w: &World, mut m: Box, mut dx: f64, mut dy: f64, mut dz: f64) -> Clipped {
    let e = Box {
        x0: m.x0 + if dx < 0.0 { dx } else { 0.0 },
        y0: m.y0 + if dy < 0.0 { dy } else { 0.0 },
        z0: m.z0 + if dz < 0.0 { dz } else { 0.0 },
        x1: m.x1 + if dx > 0.0 { dx } else { 0.0 },
        y1: m.y1 + if dy > 0.0 { dy } else { 0.0 },
        z1: m.z1 + if dz > 0.0 { dz } else { 0.0 },
    };
    let (x0, x1) = (libm::floor(e.x0) as i32, libm::floor(e.x1) as i32);
    let (mut y0, mut y1) = (libm::floor(e.y0) as i32, libm::floor(e.y1) as i32);
    let (z0, z1) = (libm::floor(e.z0) as i32, libm::floor(e.z1) as i32);
    if y0 < 0 {
        y0 = 0;
    }
    if y1 > 255 {
        y1 = 255;
    }
    // gather candidate collision boxes from the exact per-state shapes (slabs, stairs, fences, ladders, ...)
    let mut cubes = [Box { x0: 0.0, y0: 0.0, z0: 0.0, x1: 0.0, y1: 0.0, z1: 0.0 }; 192];
    let mut n = 0usize;
    for y in y0..=y1 {
        for z in z0..=z1 {
            for x in x0..=x1 {
                let sh = blocks::shape_of(w.block_state(x, y, z));
                if sh == 0 {
                    continue;
                }
                let r = &blocks::SHAPES[sh as usize];
                for k in 0..r.n as usize {
                    if n >= 192 {
                        break;
                    }
                    let q = &blocks::BOXES[r.off as usize + k];
                    cubes[n] = Box {
                        x0: x as f64 + q.x0 as f64 / 32.0,
                        y0: y as f64 + q.y0 as f64 / 32.0,
                        z0: z as f64 + q.z0 as f64 / 32.0,
                        x1: x as f64 + q.x1 as f64 / 32.0,
                        y1: y as f64 + q.y1 as f64 / 32.0,
                        z1: z as f64 + q.z1 as f64 / 32.0,
                    };
                    n += 1;
                }
            }
        }
    }
    for c in &cubes[..n] {
        dy = clip_y(c, &m, dy);
    }
    m.y0 += dy;
    m.y1 += dy;
    let z_first = libm::fabs(dz) > libm::fabs(dx);
    if z_first {
        for c in &cubes[..n] {
            dz = clip_z(c, &m, dz);
        }
        m.z0 += dz;
        m.z1 += dz;
    }
    for c in &cubes[..n] {
        dx = clip_x(c, &m, dx);
    }
    m.x0 += dx;
    m.x1 += dx;
    if !z_first {
        for c in &cubes[..n] {
            dz = clip_z(c, &m, dz);
        }
        m.z0 += dz;
        m.z1 += dz;
    }
    Clipped { dx, dy, dz, bx: m }
}

/// Teleports/moves with collision (exposed for tests / custom controllers).
pub fn physics_move(b: &mut Body, dx: f64, dy: f64, dz: f64, w: &World) {
    let (odx, ody, odz) = (dx, dy, dz);
    let start = body_box(b);
    let mut c = clip_motion(w, start, dx, dy, dz);
    let was_ground = b.on_ground;
    if (was_ground || (ody != c.dy && ody < 0.0)) && (odx != c.dx || odz != c.dz) {
        // step up
        let up = clip_motion(w, start, 0.0, STEP, 0.0);
        let st = clip_motion(w, up.bx, odx, 0.0, odz);
        let down = clip_motion(w, st.bx, 0.0, -up.dy, 0.0);
        let nh = c.dx * c.dx + c.dz * c.dz;
        let sh = st.dx * st.dx + st.dz * st.dz;
        if sh > nh {
            c.dx = st.dx;
            c.dz = st.dz;
            c.dy = up.dy + down.dy;
            c.bx = down.bx;
        }
    }
    b.x = (c.bx.x0 + c.bx.x1) / 2.0;
    b.y = c.bx.y0;
    b.z = (c.bx.z0 + c.bx.z1) / 2.0;
    b.collided_h = odx != c.dx || odz != c.dz;
    b.on_ground = ody != c.dy && ody < 0.0;
    if odx != c.dx {
        b.vx = 0.0;
    }
    if odz != c.dz {
        b.vz = 0.0;
    }
    if ody != c.dy {
        b.vy = 0.0;
    }
}

fn slipperiness(w: &World, b: &Body) -> f32 {
    let id = w.block_id(libm::floor(b.x) as i32, libm::floor(b.y) as i32 - 1, libm::floor(b.z) as i32);
    if id == 79 || id == 174 {
        0.98 // ice, packed ice
    } else if id == 165 {
        0.8 // slime
    } else {
        0.6
    }
}

/// Advance one 50 ms tick. Returns false (and does nothing) if the chunk under the player is not loaded yet.
/// `flying`: creative flight (no gravity). `walk_speed`/`fly_speed` are the ability values (defaults 0.1 / 0.05).
pub fn physics_tick(b: &mut Body, input: &Controls, w: &World, flying: bool, walk_speed: f32, fly_speed: f32) -> bool {
    if !w.column_loaded((libm::floor(b.x) as i32) >> 4, (libm::floor(b.z) as i32) >> 4) {
        return false;
    }
    // sprint/sneak state
    b.sneaking = input.sneak && !flying;
    b.sprinting = input.sprint && input.forward && !b.sneaking;
    if libm::fabs(b.vx) < 0.005 {
        b.vx = 0.0;
    }
    if libm::fabs(b.vy) < 0.005 {
        b.vy = 0.0;
    }
    if libm::fabs(b.vz) < 0.005 {
        b.vz = 0.0;
    }
    let (fx, fy, fz) = (libm::floor(b.x) as i32, libm::floor(b.y) as i32, libm::floor(b.z) as i32);
    let here = w.block_id(fx, fy, fz);
    b.in_water = here == 8 || here == 9;
    b.in_lava = here == 10 || here == 11;
    b.on_ladder = w.climbable(fx, fy, fz);

    let mut strafe = (input.left as i32 - input.right as i32) as f32 * 0.98;
    let mut fwd = (input.forward as i32 - input.back as i32) as f32 * 0.98;
    if b.sneaking {
        strafe *= 0.3;
        fwd *= 0.3;
    }
    let rad = b.yaw as f64 * (core::f64::consts::PI / 180.0);
    let (s, c) = (libm::sin(rad), libm::cos(rad));
    if b.jump_ticks > 0 {
        b.jump_ticks -= 1;
    }
    let move_rel = |b: &mut Body, friction: f32| {
        let mut d = libm::sqrtf(strafe * strafe + fwd * fwd);
        if d < 1.0 {
            d = 1.0;
        }
        let d = friction / d;
        let (sx, fz2) = (strafe * d, fwd * d);
        b.vx += sx as f64 * c - fz2 as f64 * s;
        b.vz += fz2 as f64 * c + sx as f64 * s;
    };

    if flying {
        let sp = fly_speed as f64 * if b.sprinting { 2.0 } else { 1.0 };
        if input.jump {
            b.vy += sp * 3.0;
        }
        if input.sneak {
            b.vy -= sp * 3.0;
        }
        move_rel(b, sp as f32);
        let (vx, vy, vz) = (b.vx, b.vy, b.vz);
        physics_move(b, vx, vy, vz, w);
        b.vx *= 0.91;
        b.vz *= 0.91;
        b.vy *= 0.6;
        b.on_ground = false;
        return true;
    }
    if input.jump {
        if b.in_water || b.in_lava {
            b.vy += 0.04;
        } else if b.on_ground && b.jump_ticks == 0 {
            b.vy = 0.42;
            b.jump_ticks = 10;
            if b.sprinting {
                b.vx -= libm::sin(rad) * 0.2;
                b.vz += libm::cos(rad) * 0.2;
            }
        }
    }
    if b.in_water || b.in_lava {
        let y0 = b.y;
        let drag: f32 = if b.in_lava { 0.5 } else { 0.8 };
        move_rel(b, 0.02);
        let (vx, vy, vz) = (b.vx, b.vy, b.vz);
        physics_move(b, vx, vy, vz, w);
        b.vx *= drag as f64;
        b.vy *= drag as f64;
        b.vz *= drag as f64;
        b.vy -= 0.02;
        if b.collided_h && b.vy + 0.6 - b.y + y0 > 0.0 {
            b.vy = 0.3; // climb out at the edge
        }
    } else {
        let mut f6: f32 = 0.91;
        if b.on_ground {
            f6 = slipperiness(w, b) * 0.91;
        }
        let f7 = 0.16277136f32 / (f6 * f6 * f6);
        let speed = walk_speed * if b.sprinting { 1.3 } else { 1.0 };
        move_rel(b, if b.on_ground { speed * f7 } else if b.sprinting { 0.026 } else { 0.02 });
        if b.on_ladder {
            // clamp & climb
            b.vx = b.vx.clamp(-0.15, 0.15);
            b.vz = b.vz.clamp(-0.15, 0.15);
            if b.vy < -0.15 {
                b.vy = -0.15;
            }
            if b.sneaking && b.vy < 0.0 {
                b.vy = 0.0;
            }
        }
        let (mx, mut my, mz) = (b.vx, b.vy, b.vz);
        if b.on_ladder && b.collided_h {
            my = 0.2; // climbing
        }
        b.vy = my;
        physics_move(b, mx, my, mz, w);
        if b.on_ladder && input.forward && b.collided_h {
            b.vy = 0.2;
        }
        b.vy -= 0.08;
        b.vy *= 0.98;
        f6 = 0.91;
        if b.on_ground {
            f6 = slipperiness(w, b) * 0.91;
        }
        b.vx *= f6 as f64;
        b.vz *= f6 as f64;
    }
    true
}
