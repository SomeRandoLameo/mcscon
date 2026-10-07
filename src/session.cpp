#include <mcscon/session.h>
#include <math.h>

namespace mc {

Session::Session(const Transport& t, const Buffers& b, Version v, const SessionStorage& s)
    : client_(t, b, v), chatFn_(0), chatUser_(0), packetFn_(0), packetUser_(0), eventFn_(0), eventUser_(0),
      autoRespawn_(true), rpPolicy_(ResourcePackPolicy::Accept), viewDistance_(8), brand_("mcscon"),
      lastMs_(0), accMs_(0), haveTime_(false), sentX_(0), sentY_(0), sentZ_(0), sentYaw_(0), sentPitch_(0),
      sentGround_(false), sentSneak_(false), sentSprint_(false), posTicks_(0), wasDead_(false) {
    memset(&st_, 0, sizeof st_); st_.health = 20; st_.food = 20; st_.walkSpeed = 0.1f; st_.flySpeed = 0.05f;
    world_.init(s.worldMem, s.worldMemSize, s.world);
    entities_.init(s.entities, s.maxEntities);
    players_.init(s.players, s.maxPlayers);
    client_.onPacket(trampoline, this);
}

bool Session::connect(const char* host, u16 port, const char* username) {
    world_.clear(); entities_.clear(); inv_.reset();
    memset(&st_, 0, sizeof st_); st_.health = 20; st_.food = 20; st_.walkSpeed = 0.1f; st_.flySpeed = 0.05f;
    body_ = Body(); controls = Controls(); haveTime_ = false; accMs_ = 0; wasDead_ = false;
    return client_.connectLogin(host, port, username);
}

int Session::poll() { return client_.poll(); }

bool Session::handlePacket(const Packet& p) {
    if (p.state == State::Play) {
        // component models first (they see Login/Respawn too)
        world_.handle(p); entities_.handle(p); players_.handle(p); inv_.handle(p, client_);
        cb::Login lg; cb::Respawn rs; cb::Position pos; cb::UpdateHealth hp; cb::Experience xp; cb::Abilities ab; cb::UpdateTime tm;
        cb::SpawnPosition sp; cb::GameStateChange gs; cb::Chat ch; cb::ResourcePackSend rp; cb::Difficulty df; cb::KickDisconnect kd;
        if (pkt::read(p, lg)) {
            st_.joined = true; st_.entityId = lg.entityId; st_.gameMode = lg.gameMode & 7; st_.hardcore = lg.gameMode & 8; st_.dimension = lg.dimension;
            st_.difficulty = lg.difficulty; st_.maxPlayers = lg.maxPlayers; body_ = Body(); st_.spawned = false;
            pkt::clientSettings(client_, "en_US", viewDistance_);
            u8 data[32]; Writer w(data, sizeof data); w.str(brand_); pkt::pluginMessage(client_, "MC|Brand", data, (size_t)(w.p - data));
            fire(EvJoined, lg.entityId);
        } else if (pkt::read(p, rs)) {
            st_.dimension = rs.dimension; st_.difficulty = rs.difficulty; st_.gameMode = rs.gamemode & 7; st_.health = 20; st_.spawned = false; wasDead_ = false;
            fire(EvDimensionChanged, rs.dimension); fire(EvRespawned);
        } else if (pkt::read(p, pos)) {
            u8 f = pos.flags;
            body_.x = (f & 1) ? body_.x + pos.x : pos.x; body_.y = (f & 2) ? body_.y + pos.y : pos.y; body_.z = (f & 4) ? body_.z + pos.z : pos.z;
            body_.pitch = (f & 8) ? body_.pitch + pos.pitch : pos.pitch; body_.yaw = (f & 16) ? body_.yaw + pos.yaw : pos.yaw;
            body_.vx = body_.vy = body_.vz = 0;
            pkt::teleportConfirm(client_, pos.teleportId);
            pkt::positionLook(client_, body_.x, body_.y, body_.z, body_.yaw, body_.pitch, false);   // vanilla acknowledges with the accepted position
            sentX_ = body_.x; sentY_ = body_.y; sentZ_ = body_.z; sentYaw_ = body_.yaw; sentPitch_ = body_.pitch; posTicks_ = 0;
            bool first = !st_.spawned; st_.spawned = true; fire(first ? EvSpawned : EvTeleported);
        } else if (pkt::read(p, hp)) {
            st_.health = hp.health; st_.food = hp.food; st_.saturation = hp.foodSaturation;
            if (hp.health <= 0 && !wasDead_) { wasDead_ = true; fire(EvDied); if (autoRespawn_) respawn(); }
            else if (hp.health > 0) wasDead_ = false;
        } else if (pkt::read(p, xp)) { st_.xpBar = xp.experienceBar; st_.xpLevel = xp.level; st_.xpTotal = xp.totalExperience; }
        else if (pkt::read(p, ab)) {
            st_.abilities = (u8)ab.flags; st_.flySpeed = ab.flyingSpeed; st_.walkSpeed = ab.walkingSpeed;
        } else if (pkt::read(p, tm)) { st_.worldAge = tm.age; st_.timeOfDay = tm.time; }
        else if (pkt::read(p, sp)) { st_.spawnPoint = sp.location; }
        else if (pkt::read(p, df)) { st_.difficulty = df.difficulty; }
        else if (pkt::read(p, gs)) {
            if (gs.reason == 3) { st_.gameMode = (u8)gs.gameMode; fire(EvGameModeChanged, st_.gameMode); }
            else if (gs.reason == 1) st_.raining = false; else if (gs.reason == 2) st_.raining = true; else if (gs.reason == 7) st_.rainLevel = gs.gameMode;
        } else if (pkt::read(p, ch) && chatFn_) {
            char text[512]; chatToText(ch.message.data, ch.message.len, text, sizeof text); chatFn_(chatUser_, text, ch.position, ch.message);
        } else if (pkt::read(p, rp)) {
            if (rpPolicy_ == ResourcePackPolicy::Decline) { sb::ResourcePackReceive r = {}; r.result = 1; pkt::send(client_, r); }
            else if (rpPolicy_ == ResourcePackPolicy::Accept) { sb::ResourcePackReceive r = {}; r.result = 3; pkt::send(client_, r); r.result = 0; pkt::send(client_, r); }
        } else if (pkt::read(p, kd)) { fire(EvKicked); }
    }
    if (packetFn_) packetFn_(packetUser_, p);
    return true;
}

// Mirrors EntityPlayerSP.onUpdateWalkingPlayer
void Session::sendMovement() {
    if (body_.sneaking != sentSneak_) { pkt::entityAction(client_, st_.entityId, body_.sneaking ? 0 : 1); sentSneak_ = body_.sneaking; }
    if (body_.sprinting != sentSprint_) { pkt::entityAction(client_, st_.entityId, body_.sprinting ? 3 : 4); sentSprint_ = body_.sprinting; }
    double dx = body_.x - sentX_, dy = body_.y - sentY_, dz = body_.z - sentZ_;
    bool moved = dx * dx + dy * dy + dz * dz > 9.0e-4 || ++posTicks_ >= 20;
    bool rotated = body_.yaw != sentYaw_ || body_.pitch != sentPitch_;
    if (moved && rotated) pkt::positionLook(client_, body_.x, body_.y, body_.z, body_.yaw, body_.pitch, body_.onGround);
    else if (moved) pkt::position(client_, body_.x, body_.y, body_.z, body_.onGround);
    else if (rotated) pkt::look(client_, body_.yaw, body_.pitch, body_.onGround);
    else if (sentGround_ != body_.onGround) pkt::onGround(client_, body_.onGround);
    if (moved) { sentX_ = body_.x; sentY_ = body_.y; sentZ_ = body_.z; posTicks_ = 0; }
    if (rotated) { sentYaw_ = body_.yaw; sentPitch_ = body_.pitch; }
    sentGround_ = body_.onGround;
}

void Session::tick() {
    if (!st_.spawned || st_.dead() || client_.state() != State::Play) return;
    bool flying = (st_.abilities & AbilityFlying) != 0;
    if (st_.gameMode == Spectator) flying = true;
    physicsTick(body_, controls, world_, flying, st_.walkSpeed, st_.flySpeed);
    sendMovement();
}

int Session::update(u32 nowMs) {
    if (!haveTime_) { haveTime_ = true; lastMs_ = nowMs; return 0; }
    accMs_ += nowMs - lastMs_; lastMs_ = nowMs;
    int n = 0;
    while (accMs_ >= 50 && n < 5) { accMs_ -= 50; tick(); ++n; }
    if (accMs_ >= 50) accMs_ = 0;     // do not spiral after long stalls
    return n;
}

bool Session::say(const char* m) {
    size_t n = strlen(m); bool ok = true;
    while (n) {
        size_t k = n > 256 ? 256 : n;
        if (k < n) while (k > 0 && ((u8)m[k] & 0xC0) == 0x80) --k;      // do not split UTF-8 sequences
        sb::Chat c = {}; c.message = StrView{m, (u32)k}; ok &= pkt::send(client_, c);
        m += k; n -= k;
    }
    return ok;
}

void Session::lookAt(double x, double y, double z) {
    double dx = x - body_.x, dy = y - body_.eyeY(), dz = z - body_.z, h = sqrt(dx * dx + dz * dz);
    body_.yaw = (float)(atan2(dz, dx) * 180.0 / 3.14159265358979323846 - 90.0);
    body_.pitch = (float)(-atan2(dy, h) * 180.0 / 3.14159265358979323846);
}

bool Session::breakBlockInstant(BlockPos p, i8 face) {
    return pkt::swingArm(client_) && pkt::blockDig(client_, 0, p, face) && pkt::blockDig(client_, 2, p, face);
}
bool Session::placeBlock(BlockPos against, i8 face, i32 hand) { return pkt::blockPlace(client_, against, face, hand, 0.5f, 0.5f, 0.5f); }

} // namespace mc
