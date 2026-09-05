//! Test data generator for the frame input path of `sndif_server`.
//!
//! The generator starts `THREADS_PER_CAMERA` threads for each camera. Each
//! thread sends `FRAMES_PER_THREAD` frames. It uses one TCP connection for each
//! frame.
//!
//! Each thread sends a different range of frame numbers. Thus the names of the
//! members in the zip archive are unique.
//!
//! The payload is a buffer of zeros with the size of one uncompressed frame.
//! The generator does not compress the data. Thus the server receives the
//! maximum quantity of data for each frame. This is the condition with the
//! highest load.
//!
//! The header format is the same as the format of the camera application. See
//! docs/frame-protocol.md.
//!
//! # Configuration
//!
//! An environment variable can replace each value in the configuration:
//!
//! | Variable | Default | Function |
//! |---|---|---|
//! | `SNDIF_TEST_SERVER_ADDRESS` | `127.0.0.1:8080` | The address of the server |
//! | `SNDIF_TEST_CAMERA_COUNT` | `3` | The number of cameras |
//! | `SNDIF_TEST_THREADS_PER_CAMERA` | `2` | The threads for each camera |
//! | `SNDIF_TEST_FRAMES_PER_THREAD` | `500` | The frames from each thread |
//! | `SNDIF_TEST_FRAME_WIDTH` | `2304` | The width of a frame in pixels |
//! | `SNDIF_TEST_FRAME_HEIGHT` | `2304` | The height of a frame in pixels |
//! | `SNDIF_TEST_FRAME_BYTES_PER_PX` | `2` | The bytes for each pixel |
//!
//! This example sends a small quantity of data to a different host:
//!
//! ```sh
//! SNDIF_TEST_SERVER_ADDRESS=10.0.0.5:8080 \
//! SNDIF_TEST_FRAMES_PER_THREAD=10 \
//! SNDIF_TEST_FRAME_WIDTH=256 SNDIF_TEST_FRAME_HEIGHT=256 \
//!     cargo run --release
//! ```

use std::io::prelude::*;

/// Size of the frame header in bytes. This is `sizeof(struct sendme)` with
/// 8-byte alignment.
///
/// This value is part of the format on the line. An environment variable cannot
/// change it. See `sendme.h` and docs/frame-protocol.md.
const HEADER_SIZE: usize = 32;

/// Default values. An environment variable can replace each of these values.
mod defaults {
    pub const SERVER_ADDRESS: &str = "127.0.0.1:8080";
    pub const CAMERA_COUNT: u8 = 3;
    pub const THREADS_PER_CAMERA: u32 = 2;
    pub const FRAMES_PER_THREAD: u32 = 500;
    pub const FRAME_WIDTH: usize = 2304;
    pub const FRAME_HEIGHT: usize = 2304;
    pub const FRAME_BYTES_PER_PX: usize = 2;
}

/// Reads an environment variable.
///
/// If the variable is not set, this function gives the default value. If the
/// value is not valid, this function stops the program.
fn env_or<T>(name: &str, default: T) -> T
where
    T: std::str::FromStr,
    <T as std::str::FromStr>::Err: std::fmt::Display,
{
    match std::env::var(name) {
        Ok(raw) => raw.trim().parse().unwrap_or_else(|e| {
            eprintln!("The value of {} is not correct: {:?} ({})", name, raw, e);
            std::process::exit(2);
        }),
        Err(std::env::VarError::NotPresent) => default,
        Err(e) => {
            eprintln!("The program cannot read {}: {}", name, e);
            std::process::exit(2);
        }
    }
}

/// The configuration of the generator.
struct Config {
    server_address: String,
    camera_count: u8,
    threads_per_camera: u32,
    frames_per_thread: u32,
    frame_size: usize,
}

impl Config {
    fn from_env() -> Config {
        let width: usize = env_or("SNDIF_TEST_FRAME_WIDTH", defaults::FRAME_WIDTH);
        let height: usize = env_or("SNDIF_TEST_FRAME_HEIGHT", defaults::FRAME_HEIGHT);
        let bytes_per_px: usize =
            env_or("SNDIF_TEST_FRAME_BYTES_PER_PX", defaults::FRAME_BYTES_PER_PX);

        Config {
            server_address: env_or(
                "SNDIF_TEST_SERVER_ADDRESS",
                defaults::SERVER_ADDRESS.to_string(),
            ),
            camera_count: env_or("SNDIF_TEST_CAMERA_COUNT", defaults::CAMERA_COUNT),
            threads_per_camera: env_or(
                "SNDIF_TEST_THREADS_PER_CAMERA",
                defaults::THREADS_PER_CAMERA,
            ),
            frames_per_thread: env_or(
                "SNDIF_TEST_FRAMES_PER_THREAD",
                defaults::FRAMES_PER_THREAD,
            ),
            frame_size: width * height * bytes_per_px,
        }
    }

    /// Writes the configuration in the log. Stops the program if a value is not
    /// usable.
    fn show_and_check(&self) {
        let threads = (self.camera_count as u64) * (self.threads_per_camera as u64);
        let frames = threads * (self.frames_per_thread as u64);

        println!("Configuration:");
        println!("  SNDIF_TEST_SERVER_ADDRESS     = {}", self.server_address);
        println!("  SNDIF_TEST_CAMERA_COUNT       = {}", self.camera_count);
        println!("  SNDIF_TEST_THREADS_PER_CAMERA = {}", self.threads_per_camera);
        println!("  SNDIF_TEST_FRAMES_PER_THREAD  = {}", self.frames_per_thread);
        println!("  frame size                    = {} bytes", self.frame_size);
        println!(
            "  total                         = {} threads, {} frames, {} bytes",
            threads,
            frames,
            frames * (self.frame_size as u64)
        );

        if self.frame_size == 0 {
            eprintln!("The frame size must be more than 0.");
            std::process::exit(2);
        }
        if self.frame_size > u32::MAX as usize {
            eprintln!("The frame size must not be more than {} bytes.", u32::MAX);
            std::process::exit(2);
        }
    }
}

/// Gives the number of milliseconds after the start of the Unix epoch.
fn current_timecode() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("The system clock is before the Unix epoch.")
        .as_millis() as u64
}

/// Makes the 32-byte frame header.
///
/// The bytes 1 to 3 and the bytes 28 to 31 are alignment padding. They stay at
/// zero. The receiver ignores them.
fn build_header(
    cameraid: u8,
    frameid: u32,
    burstid: u32,
    expid: u32,
    timecode: u64,
    payload_size: u32,
) -> [u8; HEADER_SIZE] {
    let mut header = [0u8; HEADER_SIZE];
    header[0] = cameraid;
    header[4..8].copy_from_slice(&frameid.to_le_bytes());
    header[8..12].copy_from_slice(&burstid.to_le_bytes());
    header[12..16].copy_from_slice(&expid.to_le_bytes());
    header[16..24].copy_from_slice(&timecode.to_le_bytes());
    header[24..28].copy_from_slice(&payload_size.to_le_bytes());
    header
}

fn main() {
    let config = Config::from_env();
    config.show_and_check();

    let mut handles = Vec::new();

    for cameraid in 0..config.camera_count {
        for thread in 0..config.threads_per_camera {
            let server_address = config.server_address.clone();
            let frames_per_thread = config.frames_per_thread;
            let frame_size = config.frame_size;

            handles.push(std::thread::spawn(move || {
                // Get the payload one time only. The generator must not stop
                // for memory allocation in the loop.
                let payload: Vec<u8> = vec![0u8; frame_size];

                // The first frame number is 1. Each thread uses a different
                // range of frame numbers.
                let first_frameid = (thread * frames_per_thread) + 1;

                for frameid in first_frameid..(first_frameid + frames_per_thread) {
                    let mut stream = std::net::TcpStream::connect(&server_address)
                        .unwrap_or_else(|e| {
                            panic!("The generator cannot connect to {}: {}", server_address, e)
                        });

                    let header = build_header(
                        cameraid,
                        frameid,
                        0, // burstid
                        0, // expid
                        current_timecode(),
                        frame_size as u32,
                    );

                    stream.write_all(&header).unwrap();
                    stream.write_all(&payload).unwrap();

                    // The half-close operation gives the end of the frame. The
                    // server reads the payload with `read_to_end`.
                    stream.shutdown(std::net::Shutdown::Write).unwrap();
                }
            }));
        }
    }

    while !handles.is_empty() {
        handles
            .pop()
            .expect("The generator cannot get a thread handle.")
            .join()
            .unwrap();
    }
}
