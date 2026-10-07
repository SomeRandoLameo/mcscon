#include <mcscon/physics.h>
#include <math.h>

namespace mc {
namespace {
struct Box { double x0, y0, z0, x1, y1, z1; };
const double kHalf = 0.3, kStep = 0.6;

Box bodyBox(const Body& b) { Box r = {b.x - kHalf, b.y, b.z - kHalf, b.x + kHalf, b.y + b.height(), b.z + kHalf}; return r; }

inline double clipX(const Box& b, const Box& m, double d) {
    if (m.y1 <= b.y0 || m.y0 >= b.y1 || m.z1 <= b.z0 || m.z0 >= b.z1) return d;
    if (d > 0 && m.x1 <= b.x0) { double c = b.x0 - m.x1; if (c < d) d = c; }
    else if (d < 0 && m.x0 >= b.x1) { double c = b.x1 - m.x0; if (c > d) d = c; }
    return d;
}
inline double clipY(const Box& b, const Box& m, double d) {
    if (m.x1 <= b.x0 || m.x0 >= b.x1 || m.z1 <= b.z0 || m.z0 >= b.z1) return d;
    if (d > 0 && m.y1 <= b.y0) { double c = b.y0 - m.y1; if (c < d) d = c; }
    else if (d < 0 && m.y0 >= b.y1) { double c = b.y1 - m.y0; if (c > d) d = c; }
    return d;
}
inline double clipZ(const Box& b, const Box& m, double d) {
    if (m.x1 <= b.x0 || m.x0 >= b.x1 || m.y1 <= b.y0 || m.y0 >= b.y1) return d;
    if (d > 0 && m.z1 <= b.z0) { double c = b.z0 - m.z1; if (c < d) d = c; }
    else if (d < 0 && m.z0 >= b.z1) { double c = b.z1 - m.z0; if (c > d) d = c; }
    return d;
}

// Clip motion against all solid blocks around the (motion-expanded) box. Order matches vanilla: Y, then X/Z.
struct Clipped { double dx, dy, dz; Box box; };
Clipped clipMotion(const World& w, Box m, double dx, double dy, double dz) {
    Box e = {m.x0 + (dx < 0 ? dx : 0), m.y0 + (dy < 0 ? dy : 0), m.z0 + (dz < 0 ? dz : 0), m.x1 + (dx > 0 ? dx : 0), m.y1 + (dy > 0 ? dy : 0), m.z1 + (dz > 0 ? dz : 0)};
    int x0 = (int)floor(e.x0), x1 = (int)floor(e.x1), y0 = (int)floor(e.y0), y1 = (int)floor(e.y1), z0 = (int)floor(e.z0), z1 = (int)floor(e.z1);
    if (y0 < 0) y0 = 0; if (y1 > 255) y1 = 255;
    // gather candidate collision boxes from the exact per-state shapes (slabs, stairs, fences, ladders, ...)
    Box cubes[192]; int n = 0;
    for (int y = y0; y <= y1; ++y) for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) {
        u8 sh = blocks::shapeOf(w.blockState(x, y, z)); if (!sh) continue;
        const blocks::ShapeRef& r = blocks::kShapes[sh];
        for (u8 k = 0; k < r.n && n < 192; ++k) {
            const blocks::Box32& q = blocks::kBoxes[r.off + k];
            Box c = {x + q.x0 / 32.0, y + q.y0 / 32.0, z + q.z0 / 32.0, x + q.x1 / 32.0, y + q.y1 / 32.0, z + q.z1 / 32.0}; cubes[n++] = c;
        }
    }
    for (int i = 0; i < n; ++i) dy = clipY(cubes[i], m, dy);
    m.y0 += dy; m.y1 += dy;
    bool zFirst = fabs(dz) > fabs(dx);
    if (zFirst) { for (int i = 0; i < n; ++i) dz = clipZ(cubes[i], m, dz); m.z0 += dz; m.z1 += dz; }
    for (int i = 0; i < n; ++i) dx = clipX(cubes[i], m, dx);
    m.x0 += dx; m.x1 += dx;
    if (!zFirst) { for (int i = 0; i < n; ++i) dz = clipZ(cubes[i], m, dz); m.z0 += dz; m.z1 += dz; }
    Clipped r = {dx, dy, dz, m}; return r;
}
} // namespace

void physicsMove(Body& b, double dx, double dy, double dz, const World& w) {
    double odx = dx, ody = dy, odz = dz;
    Box start = bodyBox(b);
    Clipped c = clipMotion(w, start, dx, dy, dz);
    bool wasGround = b.onGround;
    if ((wasGround || (ody != c.dy && ody < 0)) && (odx != c.dx || odz != c.dz)) {   // step up
        Clipped up = clipMotion(w, start, 0, kStep, 0);
        Clipped st = clipMotion(w, up.box, odx, 0, odz);
        Clipped down = clipMotion(w, st.box, 0, -(up.dy), 0);
        double nh = c.dx * c.dx + c.dz * c.dz, sh = st.dx * st.dx + st.dz * st.dz;
        if (sh > nh) { c.dx = st.dx; c.dz = st.dz; c.dy = up.dy + down.dy; c.box = down.box; }
    }
    b.x = (c.box.x0 + c.box.x1) / 2; b.y = c.box.y0; b.z = (c.box.z0 + c.box.z1) / 2;
    b.collidedH = (odx != c.dx) || (odz != c.dz);
    b.onGround = (ody != c.dy) && ody < 0;
    if (odx != c.dx) b.vx = 0;
    if (odz != c.dz) b.vz = 0;
    if (ody != c.dy) b.vy = 0;
}

static float slipperiness(const World& w, const Body& b) {
    u8 id = w.blockId((int)floor(b.x), (int)floor(b.y) - 1, (int)floor(b.z));
    if (id == 79 || id == 174) return 0.98f;      // ice, packed ice
    if (id == 165) return 0.8f;                   // slime
    return 0.6f;
}

bool physicsTick(Body& b, const Controls& in, const World& w, bool flying, float walkSpeed, float flySpeed) {
    if (!w.columnLoaded((int)floor(b.x) >> 4, (int)floor(b.z) >> 4)) return false;
    // sprint/sneak state
    b.sneaking = in.sneak && !flying;
    b.sprinting = in.sprint && in.forward && !b.sneaking;
    if (fabs(b.vx) < 0.005) b.vx = 0; if (fabs(b.vy) < 0.005) b.vy = 0; if (fabs(b.vz) < 0.005) b.vz = 0;
    int fx = (int)floor(b.x), fy = (int)floor(b.y), fz = (int)floor(b.z);
    b.inWater = w.blockId(fx, fy, fz) == 8 || w.blockId(fx, fy, fz) == 9;
    b.inLava = w.blockId(fx, fy, fz) == 10 || w.blockId(fx, fy, fz) == 11;
    b.onLadder = w.climbable(fx, fy, fz);

    float strafe = ((in.left ? 1 : 0) - (in.right ? 1 : 0)) * 0.98f, fwd = ((in.forward ? 1 : 0) - (in.back ? 1 : 0)) * 0.98f;
    if (b.sneaking) { strafe *= 0.3f; fwd *= 0.3f; }
    const double rad = b.yaw * (3.14159265358979323846 / 180.0), s = sin(rad), c = cos(rad);
    if (b.jumpTicks) --b.jumpTicks;
    auto moveRel = [&](float friction) {
        float d = strafe * strafe + fwd * fwd; d = sqrtf(d); if (d < 1.0f) d = 1.0f; d = friction / d;
        float sx = strafe * d, fz2 = fwd * d;
        b.vx += sx * c - fz2 * s; b.vz += fz2 * c + sx * s;
    };

    if (flying) {
        double sp = flySpeed * (b.sprinting ? 2.0 : 1.0);
        if (in.jump) b.vy += sp * 3.0; if (in.sneak) b.vy -= sp * 3.0;
        moveRel((float)sp);
        physicsMove(b, b.vx, b.vy, b.vz, w);
        b.vx *= 0.91; b.vz *= 0.91; b.vy *= 0.6; b.onGround = false;
        return true;
    }
    if (in.jump) {
        if (b.inWater || b.inLava) b.vy += 0.04;
        else if (b.onGround && b.jumpTicks == 0) {
            b.vy = 0.42; b.jumpTicks = 10;
            if (b.sprinting) { b.vx -= sin(rad) * 0.2; b.vz += cos(rad) * 0.2; }
        }
    }
    if (b.inWater || b.inLava) {
        double y0 = b.y; float drag = b.inLava ? 0.5f : 0.8f;
        moveRel(0.02f);
        physicsMove(b, b.vx, b.vy, b.vz, w);
        b.vx *= drag; b.vy *= drag; b.vz *= drag; b.vy -= 0.02;
        if (b.collidedH && b.vy + 0.6 - b.y + y0 > 0) b.vy = 0.3;      // climb out at the edge
    } else {
        float f6 = 0.91f; if (b.onGround) f6 = slipperiness(w, b) * 0.91f;
        float f7 = 0.16277136f / (f6 * f6 * f6);
        float speed = walkSpeed * (b.sprinting ? 1.3f : 1.0f);
        moveRel(b.onGround ? speed * f7 : (b.sprinting ? 0.026f : 0.02f));
        if (b.onLadder) {                                                  // clamp & climb
            if (b.vx < -0.15) b.vx = -0.15; if (b.vx > 0.15) b.vx = 0.15; if (b.vz < -0.15) b.vz = -0.15; if (b.vz > 0.15) b.vz = 0.15;
            if (b.vy < -0.15) b.vy = -0.15;
            if (b.sneaking && b.vy < 0) b.vy = 0;
        }
        double mx = b.vx, my = b.vy, mz = b.vz;
        if (b.onLadder && b.collidedH) my = 0.2;                          // climbing
        b.vy = my; physicsMove(b, mx, my, mz, w);
        if (b.onLadder && in.forward && b.collidedH) b.vy = 0.2;
        b.vy -= 0.08; b.vy *= 0.98;
        f6 = 0.91f; if (b.onGround) f6 = slipperiness(w, b) * 0.91f;
        b.vx *= f6; b.vz *= f6;
    }
    return true;
}

} // namespace mc
