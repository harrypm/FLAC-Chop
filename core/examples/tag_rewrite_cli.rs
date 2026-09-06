//! Isolated tag-rewrite repro: `tag_rewrite_cli <file.flac> <rf|audio>`
//! Runs `tags::rewrite_cut_tags` on the given file and reports the result.

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: tag_rewrite_cli <file.flac> <rf|audio>");
        std::process::exit(2);
    }
    let is_rf = args[2] == "rf";
    match flac_chop_core::tags::rewrite_cut_tags(Path::new(&args[1]), is_rf) {
        Ok(()) => println!("rewrite ok"),
        Err(e) => {
            eprintln!("rewrite failed: {e}");
            std::process::exit(1);
        }
    }
}
