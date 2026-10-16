//! World model: chunk decoding, palette growth, edits, packets, memory exhaustion.
mod common;
use common::*;
use mcscon::ids::PlayCb;
use mcscon::world::{World, WorldConfig};

fn feed(w: &mut World, pkt: &[u8], id: PlayCb) -> bool {
    w.handle(&mk(pkt, id))
}

#[test]
fn chunk_decoding_and_edits() {
    let mut mem = vec![0u8; 512 * 1024];
    let mut w = World::new(&mut mem, WorldConfig { max_columns: 16, store_light: true, store_biomes: true });
    let mut data = Vec::new();
    // section 0: bpb 4 w/ 12-entry palette; section 2: bpb 8 w/ 200 entries; section 5: bpb 13 direct
    let p4: Vec<u16> = (0..12).map(|i| ref_state(0, i)).collect();
    section(&mut data, 4, &p4, |i| (i % 12) as u64, true);
    let p8: Vec<u16> = (0..200).map(|i| ((i << 4) | 3) as u16).collect();
    section(&mut data, 8, &p8, |i| ((i * 7) % 200) as u64, true);
    section(&mut data, 13, &[], |i| ((((i * 13) % 250) << 4) | (i & 15)) as u64, true);
    data.extend((0..256).map(|i| (i % 40) as u8)); // biomes
    assert!(feed(&mut w, &map_chunk(-3, 7, true, (1 << 0) | (1 << 2) | (1 << 5), &data), PlayCb::MapChunk));
    assert!(w.loaded_columns() == 1 && w.column_loaded(-3, 7) && !w.column_loaded(0, 0));
    let mut bad = 0;
    for i in 0..4096i32 {
        let (x, z, y) = (-48 + (i & 15), 112 + ((i >> 4) & 15), i >> 8);
        if w.block_state(x, y, z) != p4[i as usize % 12] {
            bad += 1;
        }
        if w.block_state(x, 32 + y, z) != p8[(i as usize * 7) % 200] {
            bad += 1;
        }
        if w.block_state(x, 80 + y, z) != ((((i as usize * 13) % 250) << 4) | (i as usize & 15)) as u16 {
            bad += 1;
        }
        if w.block_state(x, 16 + y, z) != 0 {
            bad += 1; // section 1 absent -> air
        }
    }
    assert_eq!(bad, 0);
    assert_eq!(w.biome(-48 + 5, 112 + 2), ((2 * 16 + 5) % 40) as u8);
    assert!(w.block_light(-48, 0, 112) == 5 && w.sky_light(-48, 0, 112) == 15 && w.sky_light(-48, 16, 112) == 15);
    assert!(w.highest_block(-48, 112) >= 80);
    assert!(!w.is_loaded(0, 0, 0) && w.block_state(0, 0, 0) == 0);

    // random edits checked against a shadow copy; forces palette growth 4 -> 5 -> ... -> 13 bpb
    let mut shadow: Vec<u16> = (0..4096).map(|i| p4[i % 12]).collect();
    let mut seed = 9u32;
    let mut rnd = |m: u32| {
        seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
        (seed >> 8) % m
    };
    for k in 0..6000 {
        let i = rnd(4096) as i32;
        let st = ((rnd(220) << 4) | rnd(16)) as u16;
        assert!(w.set_block_state(-48 + (i & 15), i >> 8, 112 + ((i >> 4) & 15), st));
        shadow[i as usize] = st;
        if k % 997 == 0 {
            for j in 0..4096i32 {
                assert_eq!(w.block_state(-48 + (j & 15), j >> 8, 112 + ((j >> 4) & 15)), shadow[j as usize]);
            }
        }
    }
    for j in 0..4096i32 {
        assert_eq!(w.block_state(-48 + (j & 15), j >> 8, 112 + ((j >> 4) & 15)), shadow[j as usize]);
    }
    assert_eq!(w.block_light(-48, 0, 112), 5); // light survived the re-packing
    // writing into an absent section allocates it
    assert!(w.set_block_state(-47, 20, 113, (4 << 4) | 2));
    assert!(w.block_state(-47, 20, 113) == ((4 << 4) | 2) && w.block_state(-47, 21, 113) == 0);
    // partial update replaces only listed sections; unload frees everything
    let mut d2 = Vec::new();
    section(&mut d2, 4, &[9 << 4], |_| 0, true);
    assert!(feed(&mut w, &map_chunk(-3, 7, false, 1, &d2), PlayCb::MapChunk));
    assert!(w.block_state(-48, 0, 112) == (9 << 4) && w.block_state(-48, 32, 112) != 0);
    let mut un = Vec::new();
    varint(&mut un, 0x1D);
    un.extend_from_slice(&(-3i32).to_be_bytes());
    un.extend_from_slice(&7i32.to_be_bytes());
    assert!(feed(&mut w, &un, PlayCb::UnloadChunk));
    assert!(w.loaded_columns() == 0 && w.arena_used() == 0);
}

#[test]
fn world_packets() {
    let mut mem = vec![0u8; 256 * 1024];
    let mut w = World::new(&mut mem, WorldConfig { max_columns: 4, store_light: false, store_biomes: false });
    let mut data = Vec::new();
    section(&mut data, 4, &[0, 1 << 4], |i| (i < 256) as u64, true);
    feed(&mut w, &map_chunk(0, 0, true, 1, &data), PlayCb::MapChunk); // biome bytes missing -> must be rejected
    assert_eq!(w.loaded_columns(), 0);
    data.extend(std::iter::repeat(1).take(256));
    feed(&mut w, &map_chunk(0, 0, true, 1, &data), PlayCb::MapChunk);
    assert!(w.loaded_columns() == 1 && w.block_state(3, 0, 3) == (1 << 4) && w.block_state(3, 1, 3) == 0);
    // BlockChange
    let mut bc = Vec::new();
    varint(&mut bc, 0x0B);
    bc.extend_from_slice(&pos_bytes(3, 5, 4));
    varint(&mut bc, (35 << 4) | 14);
    assert!(feed(&mut w, &bc, PlayCb::BlockChange) && w.block_state(3, 5, 4) == ((35 << 4) | 14));
    // MultiBlockChange: chunk (0,0), two records
    let mut mb = Vec::new();
    varint(&mut mb, 0x10);
    mb.extend_from_slice(&0i32.to_be_bytes());
    mb.extend_from_slice(&0i32.to_be_bytes());
    varint(&mut mb, 2);
    mb.push((1 << 4) | 2);
    mb.push(10);
    varint(&mut mb, 20 << 4);
    mb.push((15 << 4) | 15);
    mb.push(255);
    varint(&mut mb, (7 << 4) | 1);
    assert!(feed(&mut w, &mb, PlayCb::MultiBlockChange));
    assert!(w.block_state(1, 10, 2) == (20 << 4) && w.block_state(15, 255, 15) == ((7 << 4) | 1));
    // Explosion clears blocks relative to the floored center
    let mut ex = Vec::new();
    varint(&mut ex, 0x1C);
    for f in [3.5f32, 0.5, 3.5, 4.0] {
        ex.extend_from_slice(&f.to_be_bytes());
    }
    ex.extend_from_slice(&2i32.to_be_bytes());
    ex.extend_from_slice(&[0, 0, 0, 1, 0, 0]);
    for _ in 0..3 {
        ex.extend_from_slice(&0f32.to_be_bytes());
    }
    assert!(feed(&mut w, &ex, PlayCb::Explosion));
    assert!(w.block_state(3, 0, 3) == 0 && w.block_state(4, 0, 3) == 0 && w.block_state(5, 0, 3) == (1 << 4));
    // table full: 4 columns max, a 5th is dropped
    for i in 1..=5 {
        feed(&mut w, &map_chunk(i, 0, true, 0, &[0u8; 256]), PlayCb::MapChunk);
    }
    assert_eq!(w.loaded_columns(), 4);
}

/// Memory exhaustion must degrade (missing sections), not crash.
#[test]
fn memory_exhaustion_degrades() {
    let mut tiny = vec![0u8; 4096 + 4 * 80 + 8];
    let mut w = World::new(&mut tiny, WorldConfig { max_columns: 4, store_light: false, store_biomes: false });
    let mut big = Vec::new();
    for _ in 0..8 {
        section(&mut big, 8, &[1 << 4; 256], |i| (i & 255) as u64, true);
    }
    big.extend(std::iter::repeat(0).take(256));
    feed(&mut w, &map_chunk(0, 0, true, 0xFF, &big), PlayCb::MapChunk);
    // also: a world with no memory at all
    let mut none: [u8; 0] = [];
    let mut e = World::new(&mut none, WorldConfig::default());
    assert!(!feed(&mut e, &map_chunk(0, 0, true, 0, &[0u8; 256]), PlayCb::MapChunk) || e.loaded_columns() == 0);
    assert_eq!(e.block_state(0, 0, 0), 0);
}
