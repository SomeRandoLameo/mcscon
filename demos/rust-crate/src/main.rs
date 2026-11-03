use mcscon::session::SessionStorage;
use mcscon::world::WorldConfig;
use mcscon::Version;

fn main() {
    // Smoke test of the dependency: uses the crate's types without connecting anywhere.
    let _ = (Version::V1_12_2, std::mem::size_of::<SessionStorage>(), std::mem::size_of::<WorldConfig>());
    println!("mcscon linked as a Rust dependency");
}
