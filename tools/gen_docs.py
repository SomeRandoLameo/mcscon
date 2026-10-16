#!/usr/bin/env python3
"""Generates docs/packet-reference.md from the generated Rust sources (src/ids.rs, src/packets/{cb,sb}.rs).
usage: python3 tools/gen_docs.py > docs/packet-reference.md"""
import re, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ids = open(f"{root}/src/ids.rs").read()
cb = open(f"{root}/src/packets/cb.rs").read()
sb = open(f"{root}/src/packets/sb.rs").read()
TABLES = [("1.10", "V1_10"), ("1.11 / 1.11.2", "V1_11"), ("1.12", "V1_12"), ("1.12.1 / 1.12.2", "V1_12_2")]

def names(enum):
    return re.search(r"pub enum %s \{ (.*?) \}" % enum, ids).group(1).split(", ")
def a2w(prefix, ver):
    m = re.search(r"static %s_%s_A2W: \[u8; \d+\] = \[(.*?)\];" % (prefix, ver), ids)
    return [None if x.strip() == "0xFF" else int(x) for x in m.group(1).split(",")]
def structs(src):
    out = {}
    for m in re.finditer(r"pub struct (\w+)(<'a>)? \{\n(.*?)\n\}", src, re.S):
        fields = re.findall(r"pub (\w+): (.+?),\n?", m.group(3) + "\n")
        out[m.group(1)] = fields
    return out

CB = {
 "Abilities": "Player abilities: invulnerable / flying / may fly / creative flags and the fly and walk speeds.",
 "Advancements": "1.12+: advancement tree updates (added, removed, progress).",
 "Animation": "An entity plays an animation (swing arm, take damage, leave bed, critical effect ...).",
 "AttachEntity": "Leash: attaches an entity to another entity (a leash holder).",
 "Bed": "An entity went to sleep in a bed at the given position.",
 "BlockAction": "Block event: piston extend/retract, note block, chest lid ...",
 "BlockBreakAnimation": "Progress of a block being broken by an entity (0-9 cracks, -1 = reset).",
 "BlockChange": "A single block changed.",
 "BossBar": "Boss bar add / remove / update (health, title, color, style, flags).",
 "Camera": "Spectator camera is set to an entity.",
 "Chat": "A chat message (JSON text component) with a position: 0 chat, 1 system, 2 action bar.",
 "CloseWindow": "The server closes a window.",
 "Collect": "An item or experience orb is picked up by an entity.",
 "CombatEvent": "Combat state: enter, end, or the player died (with the death message).",
 "CraftProgressBar": "Window property update: furnace progress, enchantment levels, brewing time, ...",
 "CraftRecipeResponse": "1.12+: ghost recipe shown in a crafting window.",
 "CustomPayload": "Plugin message on a channel (e.g. `MC|Brand`).",
 "Difficulty": "Server difficulty changed.",
 "Entity": "Entity base packet (no movement); only sent by some plugins.",
 "EntityDestroy": "Entities are removed from the world.",
 "EntityEffect": "A potion effect is applied to an entity.",
 "EntityEquipment": "An entity's equipment slot (hand, armor) changed.",
 "EntityHeadRotation": "An entity's head yaw changed.",
 "EntityLook": "An entity rotated.",
 "EntityMetadata": "An entity's metadata (flags, health, name, item, ...) changed.",
 "EntityMoveLook": "An entity moved a small distance and rotated.",
 "EntityStatus": "Entity status byte (hurt, died, wolf shaking, ...).",
 "EntityTeleport": "An entity moved to an absolute position.",
 "EntityUpdateAttributes": "Entity attributes (max health, speed, ...) with modifiers.",
 "EntityVelocity": "An entity's velocity changed (knockback).",
 "Experience": "Experience bar, level and total experience.",
 "Explosion": "An explosion happened: center, radius, destroyed blocks and the player knockback.",
 "GameStateChange": "Game state: invalid bed, rain begin/end, game mode change, credits, rain/thunder levels, ...",
 "HeldItemSlot": "The server selects the hotbar slot (0-8).",
 "KeepAlive": "Keep-alive ping; must be answered (the client does this automatically).",
 "KickDisconnect": "The server disconnects the player (JSON reason).",
 "Login": "Join Game: entity id, game mode, dimension, difficulty, max players, level type.",
 "Map": "Map item data (icons and pixel patches).",
 "MapChunk": "A chunk column (all or a partial set of 16x16x16 sections, optionally biomes) plus block entities.",
 "MultiBlockChange": "Several blocks changed inside one chunk column section.",
 "NamedEntitySpawn": "A player entity appears.",
 "NamedSoundEffect": "Play a sound by name at a position.",
 "OpenSignEntity": "Open the sign editor at a position.",
 "OpenWindow": "A window (chest, furnace, crafting table, villager, ...) opens.",
 "PlayerInfo": "Tab list: add players, change game mode / ping / display name, remove players.",
 "PlayerlistHeader": "Tab list header and footer text.",
 "Position": "The server moves the player (login, teleport, anti-cheat correction); must be confirmed (handled by `Session`).",
 "RelEntityMove": "An entity moved a small distance.",
 "RemoveEntityEffect": "A potion effect ended.",
 "ResourcePackSend": "The server offers a resource pack (URL + SHA-1).",
 "Respawn": "Respawn / dimension change: dimension, difficulty, game mode, level type.",
 "ScoreboardDisplayObjective": "Which objective is shown in which scoreboard slot.",
 "ScoreboardObjective": "Scoreboard objective create / remove / update.",
 "ScoreboardScore": "Scoreboard score update / remove.",
 "SelectAdvancementTab": "1.12+: show an advancement tab.",
 "SetCooldown": "An item (e.g. ender pearl) is on cooldown for N ticks.",
 "SetPassengers": "The passengers riding an entity (boats, horses, minecarts).",
 "SetSlot": "One slot of a window (or the cursor, window -1) changed.",
 "SoundEffect": "Play a sound by id at a position.",
 "SpawnEntity": "An object entity (item frames, arrows, boats, primed TNT, ...) appears.",
 "SpawnEntityExperienceOrb": "An experience orb appears.",
 "SpawnEntityLiving": "A mob appears.",
 "SpawnEntityPainting": "A painting appears.",
 "SpawnEntityWeather": "A lightning bolt appears.",
 "SpawnPosition": "World spawn / compass target.",
 "Statistics": "Statistics and achievements counters.",
 "TabComplete": "Tab-completion result for a typed command or chat prefix.",
 "Teams": "Scoreboard team create / remove / update / add or remove members.",
 "TileEntityData": "Block entity data (sign text, banner, spawner, ...) as NBT.",
 "Title": "Title / subtitle / action bar text and timings.",
 "Transaction": "Confirmation (or rejection) of a window click.",
 "UnloadChunk": "A chunk column is unloaded.",
 "UnlockRecipes": "1.12+: recipe book update.",
 "UpdateHealth": "Health, food and saturation.",
 "UpdateTime": "World age and time of day.",
 "VehicleMove": "The server moves the vehicle the player rides.",
 "WindowItems": "All slots of a window.",
 "WorldBorder": "World border: size, center, warning distances, lerp.",
 "WorldEvent": "World event: door sounds, record playing, splash potion effects, ...",
 "WorldParticles": "Spawn particles.",
}
SB = {
 "Abilities": "The client's flying state changed.",
 "AdvancementTab": "1.12+: open or close an advancement tab.",
 "ArmAnimation": "Swing an arm.",
 "BlockDig": "Digging: start / cancel / finish, drop item, release use item, swap hands.",
 "BlockPlace": "Right-click a block face (place a block / interact).",
 "Chat": "Send a chat message or a `/command`.",
 "ClientCommand": "Respawn request (action 0) or open the statistics screen (action 1).",
 "CloseWindow": "The client closes a window.",
 "CraftRecipeRequest": "1.12+: request a recipe to be placed in the crafting grid.",
 "CraftingBookData": "Recipe book state / displayed recipe.",
 "CustomPayload": "Plugin message on a channel.",
 "EnchantItem": "Click an enchantment in an enchanting table.",
 "EntityAction": "Sneak / sprint / leave bed / horse jump / open horse inventory / elytra.",
 "Flying": "Position-less update that only carries the on-ground flag.",
 "HeldItemSlot": "Select a hotbar slot (0-8).",
 "KeepAlive": "Answer to the server's keep-alive.",
 "Look": "Rotation update.",
 "Position": "Position update.",
 "PositionLook": "Position + rotation update.",
 "PrepareCraftingGrid": "Pre-1.12 recipe auto-fill request (not used in 1.10 - 1.12.2 vanilla).",
 "ResourcePackReceive": "Resource pack status: 0 loaded, 1 declined, 2 failed, 3 accepted.",
 "SetCreativeSlot": "Creative mode: put an item stack into a slot.",
 "Settings": "Client settings: locale, view distance, chat mode, skin parts, main hand.",
 "Spectate": "Spectator: teleport to an entity (by UUID).",
 "SteerBoat": "Paddle state of a boat.",
 "SteerVehicle": "Steering input while riding.",
 "TabComplete": "Request tab completions.",
 "TeleportConfirm": "Confirm a `Position` teleport id from the server.",
 "Transaction": "Confirm a window transaction the server rejected.",
 "UpdateSign": "Finish editing a sign (4 lines).",
 "UseEntity": "Interact (0), attack (1) or interact-at (2) an entity.",
 "UseItem": "Use the held item (bow, food, ender pearl, ...).",
 "VehicleMove": "Move the ridden vehicle.",
 "WindowClick": "Click in a window (mode: normal, shift, hotbar swap, drop, drag, double).",
}

def section(title, enum, prefix, src, doc, intro):
    ns = names(enum)
    pfx = "cb" if enum == "PlayCb" else "sb"
    st = structs(src)
    tabs = [a2w(prefix, v) for _, v in TABLES]
    o = [f"## {title}", "", intro, ""]
    o.append("| Packet | " + " | ".join(l for l, _ in TABLES) + " |")
    o.append("|---|" + "---|" * len(TABLES))
    for i, n in enumerate(ns):
        o.append(f"| [`{n}`](#{pfx}{n.lower()}) | " + " | ".join(("`0x%02X`" % t[i]) if i < len(t) and t[i] is not None else "–" for t in tabs) + " |")
    o.append("")
    for n in ns:
        o += [f"### {pfx}::{n}", "", doc.get(n, ""), ""]
        f = st.get(n)
        if f is None:
            o.append("_No fields._" if False else "_Fields: see `src/packets/%s.rs`._" % ("cb" if enum == "PlayCb" else "sb"))
        elif not f:
            o.append("_No fields._")
        else:
            o += ["| Field | Rust type |", "|---|---|"] + [f"| `{a}` | `{b}` |" for a, b in f]
        o.append("")
    return o

out = ["# Packet reference (Play state, 1.10 - 1.12.2)", "",
       "_Generated by `tools/gen_docs.py` from the generated sources. Do not edit by hand._", "",
       "Each packet is a Rust struct in `mcscon::packets::cb` (clientbound, decoded) or `mcscon::packets::sb` (serverbound, encoded). "
       "Field names are `snake_case` versions of the [minecraft-data](https://github.com/PrismarineJS/minecraft-data) names. "
       "Where versions differ in layout, the struct is the union of all layouts; fields a version does not have keep their default (0) on decode and are not written on encode. "
       "Types: `Str` UTF-8 view, `Bytes` raw/NBT view, `Slot` item stack, `Array<T>` lazily iterated validated array, `MetaView` entity metadata, `Option<T>` an optional field, `BlockPos` block coordinates, `Uuid` 16 bytes. "
       "Views borrow the receive buffer and are valid only inside the packet handler. See [packets](rust/packets.md) and [wire types](rust/wire.md).", "",
       "The tables list the wire packet id (hex) per id table; 1.11.2 uses the 1.11 table and 1.12.1 the 1.12.2 table. `–` = the packet does not exist in that version. Nested helper structs (for example array elements) are defined next to their packet in `src/packets/`.", ""]
out += section("Clientbound", "PlayCb", "PLAY_CB", cb, CB, "Packets the server sends. Decode with `packets::read::<cb::Name>(&packet)`.")
out += section("Serverbound", "PlaySb", "PLAY_SB", sb, SB, "Packets the client sends. Encode with `packets::send(&mut link, &sb::Name { .. })`.")
print("\n".join(out))
