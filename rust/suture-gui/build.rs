fn main() {
    // Only the optional Slint backend compiles a .slint file. The default build
    // (hand-written rasteriser) does not touch slint-build at all.
    //
    // When the `slint` feature IS on, EmbedForSoftwareRenderer is mandatory for a
    // no_std target: without it the software renderer panics with
    // "No font fallback found" the first time it draws text, and it is also what
    // turns the imported .ttf into build-time bitmap glyphs (which is why the
    // glyph set is fixed at build time).
    if std::env::var_os("CARGO_FEATURE_SLINT").is_none() {
        return;
    }
    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer);
    slint_build::compile_with_config("ui/main_v2.slint", config)
        .expect("slint build failed: ui/main_v2.slint");
    println!("cargo:rerun-if-changed=ui/main_v2.slint");
}
