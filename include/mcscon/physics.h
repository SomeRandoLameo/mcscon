// Player movement physics replicating vanilla 1.10-1.12.2 (gravity, drag, friction, AABB collision, step-up, water, ladders).
// Block collision is approximated by full unit cubes for every solid block in blocks_gen.h.
#pragma once
#include "world.h"

namespace mc {

struct Controls { bool forward = false, back = false, left = false, right = false, jump = false, sneak = false, sprint = false; };

struct Body {
    double x = 0, y = 0, z = 0;           // feet position
    double vx = 0, vy = 0, vz = 0;        // blocks per tick
    float yaw = 0, pitch = 0;             // degrees; yaw 0 = +Z (south), 90 = -X (west)
    bool onGround = false, collidedH = false, inWater = false, inLava = false, onLadder = false;
    u8 jumpTicks = 0;
    bool sneaking = false, sprinting = false;
    double height() const { return sneaking ? 1.65 : 1.8; }
    double eyeY() const { return y + (sneaking ? 1.54 : 1.62); }
};

// Advance one 50 ms tick. Returns false (and does nothing) if the chunk under the player is not loaded yet.
// `flying`: creative flight (no gravity). `walkSpeed`/`flySpeed` are the ability values (defaults 0.1 / 0.05).
bool physicsTick(Body& b, const Controls& in, const World& w, bool flying = false, float walkSpeed = 0.1f, float flySpeed = 0.05f);

// Teleports/moves with collision (exposed for tests / custom controllers).
void physicsMove(Body& b, double dx, double dy, double dz, const World& w);

} // namespace mc
