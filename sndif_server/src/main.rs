//! Frame input and archive server for the plSCM system.
//!
//! This program operates on the storage host. It receives compressed camera
//! frames on port 8080. It sorts them by camera number. Then it writes each
//! frame to one zip archive for each channel. Port 8090 sets the name of the
//! archive.
//!
//! # Threads
//!
//! * 10 parser threads (`SERVER_THREADS`) get the open sockets from a shared
//!   channel. Each thread reads the header of 32 bytes. Then it reads the
//!   payload to the end of the data. Then it sends the frame to the writer
//!   thread for that camera.
//! * 3 writer threads (`CAMERA_COUNT`). Each thread controls one `ZipWriter`.
//!   Before each frame, the thread reads its name channel. If a new name is
//!   available, the thread closes the zip file. Then it opens the file
//!   `<name>_ch<N>.zip`.
//! * 1 control thread gives the HTTP endpoint for the archive name.
//! * 1 listener thread accepts the connections on port 8080. It operates in
//!   non-blocking mode. Thus it can find the Ctrl-C signal between two
//!   connections.
//!
//! # Format on the line
//!
//! The protocol sends one frame on each connection. The client writes a header
//! of 32 bytes with little-endian values. Then it writes the payload with zstd
//! compression. Then it closes one half of the socket. The half-close operation
//! gives the end of the frame. This is why the server reads the payload with
//! `read_to_end`.
//!
//! The values in the header add up to 25 bytes. But the header is 32 bytes,
//! because the transmitter sends a C structure. A space of 3 bytes after
//! `cameraid` aligns the u32 values. The u64 time value needs 8-byte alignment.
//! The `sizeof` operator then gives 32 bytes.
//!
//! **CAUTION: The format on the line changes with the ABI.** If you change
//! `sendme.h` on the camera PC, you must also change the offsets below. Do the
//! two changes in the same commit.
//!
//! The values `burstid` and `expid` are always 0. The server reads them and
//! writes them in the log. It does not keep them.
//!
//! # Shutdown
//!
//! Use Ctrl-C to stop this server. The handler stops the listener thread. Then
//! the frame channels disconnect. Then each writer thread calls `zip.finish()`
//! and writes the central directory of the zip file. Give the server enough
//! time to write all the frames in its queue. If you use a different method,
//! the archives are not complete.
//!
//! See docs/frame-protocol.md.

use std::io::Write;
use std::io::Read;

use std::io::prelude::*;

use byteorder::ReadBytesExt;

/// Default values. An environment variable can replace each of these values.
mod defaults {
    pub const FRAME_ADDRESS: &str = "0.0.0.0:8080";
    pub const CONTROL_ADDRESS: &str = "0.0.0.0:8090";
    pub const CAMERA_COUNT: u8 = 3;
    pub const SERVER_THREADS: u32 = 10;
    pub const READ_BUFFER_SIZE: usize = 10485760; // ~10 MB
    pub const OUTPUT_DIR: &str = ".";
    pub const ARCHIVE_NAME: &str = "DEFAULT";
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

/// The configuration of the server.
struct Config {
    /// The address for the frame input port. `SNDIF_FRAME_ADDRESS`
    frame_address: String,
    /// The address for the archive name port. `SNDIF_CONTROL_ADDRESS`
    control_address: String,
    /// The number of cameras and thus the number of writer threads.
    /// `SNDIF_CAMERA_COUNT`
    camera_count: u8,
    /// The number of threads that parse the frames. `SNDIF_SERVER_THREADS`
    server_threads: u32,
    /// The first size of the buffer for one frame. `SNDIF_READ_BUFFER_SIZE`
    read_buffer_size: usize,
    /// The directory for the zip archives. `SNDIF_OUTPUT_DIR`
    output_dir: std::path::PathBuf,
    /// The name of the archive before the first name on the control port.
    /// `SNDIF_ARCHIVE_NAME`
    archive_name: String,
}

impl Config {
    fn from_env() -> Config {
        Config {
            frame_address: env_or("SNDIF_FRAME_ADDRESS", defaults::FRAME_ADDRESS.to_string()),
            control_address: env_or("SNDIF_CONTROL_ADDRESS", defaults::CONTROL_ADDRESS.to_string()),
            camera_count: env_or("SNDIF_CAMERA_COUNT", defaults::CAMERA_COUNT),
            server_threads: env_or("SNDIF_SERVER_THREADS", defaults::SERVER_THREADS),
            read_buffer_size: env_or("SNDIF_READ_BUFFER_SIZE", defaults::READ_BUFFER_SIZE),
            output_dir: std::path::PathBuf::from(env_or(
                "SNDIF_OUTPUT_DIR",
                defaults::OUTPUT_DIR.to_string(),
            )),
            archive_name: env_or("SNDIF_ARCHIVE_NAME", defaults::ARCHIVE_NAME.to_string()),
        }
    }

    /// Writes the configuration in the log. Stops the program if a value is not
    /// usable.
    fn show_and_check(&self) {
        println!("Configuration:");
        println!("  SNDIF_FRAME_ADDRESS    = {}", self.frame_address);
        println!("  SNDIF_CONTROL_ADDRESS  = {}", self.control_address);
        println!("  SNDIF_CAMERA_COUNT     = {}", self.camera_count);
        println!("  SNDIF_SERVER_THREADS   = {}", self.server_threads);
        println!("  SNDIF_READ_BUFFER_SIZE = {}", self.read_buffer_size);
        println!("  SNDIF_OUTPUT_DIR       = {}", self.output_dir.display());
        println!("  SNDIF_ARCHIVE_NAME     = {}", self.archive_name);

        if self.camera_count == 0 {
            eprintln!("SNDIF_CAMERA_COUNT must be more than 0.");
            std::process::exit(2);
        }
        if self.server_threads == 0 {
            eprintln!("SNDIF_SERVER_THREADS must be more than 0.");
            std::process::exit(2);
        }
    }

    /// Makes the output directory if it is not there.
    ///
    /// The program does this after the test of the two addresses. Thus an
    /// incorrect address does not make an empty directory.
    fn prepare_output_dir(&self) {
        if let Err(e) = std::fs::create_dir_all(&self.output_dir) {
            eprintln!(
                "The program cannot use the directory {}: {}",
                self.output_dir.display(),
                e
            );
            std::process::exit(2);
        }
    }

    /// Gives the full path of the archive for one camera.
    fn archive_path(output_dir: &std::path::Path, name: &str, cameraid: u8) -> std::path::PathBuf {
        output_dir.join(format!("{}_ch{}.zip", name, cameraid))
    }
}

struct Frame {
    cameraid: u8,
    frameid: u32,
    timecode: u64,
    data: Vec<u8>
}

/// Opens a listener on one address.
///
/// This function stops the program if the address is not usable. The program
/// does this test before it starts the threads. Thus an incorrect address stops
/// the program immediately.
fn bind_or_exit(address: &str) -> std::net::TcpListener {
    std::net::TcpListener::bind(address).unwrap_or_else(|e| {
        eprintln!("The program cannot use the address {}: {}", address, e);
        std::process::exit(2);
    })
}

fn main() {
    let config = Config::from_env();
    config.show_and_check();

    // Open the two listeners before the program starts the threads and makes
    // the archive files.
    let frame_listener = bind_or_exit(&config.frame_address);
    let control_listener = bind_or_exit(&config.control_address);

    config.prepare_output_dir();

    let (kill_sender, kill_reciever) = crossbeam_channel::unbounded();
    let (tcp_sender, tcp_reciever) = crossbeam_channel::unbounded();
    let mut frame_sender_channels = Vec::new();
    let mut frame_receiver_channels = Vec::new();
    let mut expt_sender_channels: Vec<crossbeam_channel::Sender<String>> = Vec::new();
    let mut expt_receiver_channels = Vec::new();

    for _ in 0..config.camera_count {
        let (s, r) = crossbeam_channel::unbounded();
        frame_sender_channels.push(s);
        frame_receiver_channels.push(r);

        let (s, r) = crossbeam_channel::unbounded();
        //s.send("DEFAULT".to_string());
        expt_sender_channels.push(s);
        expt_receiver_channels.push(r);
    }

    // Creates server threads for recieving data
    let mut server_handles = Vec::new();
    for threadnum in 0..config.server_threads {
        let mut new_senders: Vec<crossbeam_channel::Sender<Frame>> = frame_sender_channels.iter().map(|x| (*x).clone()).collect();
        let tcp_reciever = tcp_reciever.clone();
        let read_buffer_size = config.read_buffer_size;
        server_handles.push( std::thread::spawn(move || {
            for mut stream in tcp_reciever.iter() {
                //println!("Thread {} got connection...", threadnum);

                let mut buf_reader = std::io::BufReader::new(&mut stream);

                /*
                typedef uint32_t sendme_int;
                typedef uint64_t sendme_timecodet; //ms since epoch

                struct sendme {
                    uint8_t cameraid;     /// 1
                    sendme_int frameid;   /// 32 / 8 = 4
                    sendme_int burstid;   /// 32 / 8 = 4
                    sendme_int expid;     /// 32 / 8 = 4
                    sendme_timecodet timecode; /// 64 / 8 = 8
                    sendme_int payload_size; // 32 / 8 = 4
                    /// ----------------------------------------
                    ///                       ==> 25 ? 

                    Byte map: 
                    [
                    255     0       0       0       254     254     254     254   // 3-byte space
                    253     253     253     253     252     252     252     252
                    251     251     251     251     251     251     251     251
                    250     250     250     250     0       0       0       0     // blank bytes at end
                    ]
                };
                */

                // The values add up to 25 bytes. But the header is 32 bytes, because
                // the transmitter sends a C structure with 8-byte alignment.
                const HEADER_BUFFER_SIZE: usize = 32;
                let mut header_buffer: [u8; HEADER_BUFFER_SIZE] =  std::default::Default::default();
                buf_reader.read_exact(&mut header_buffer).unwrap();

                //println!("{:?}", header_buffer);
                
                let mut rdr = std::io::Cursor::new(header_buffer);
                let cameraid_in = rdr.read_u8().unwrap();

                for _x in 0..3 { // read through the padding
                    rdr.read_u8().unwrap();
                }

                let frameid_in = rdr.read_u32::<byteorder::LittleEndian>().unwrap();
                let burstid_in = rdr.read_u32::<byteorder::LittleEndian>().unwrap();
                let exptid_in = rdr.read_u32::<byteorder::LittleEndian>().unwrap();
                let timecode_in = rdr.read_u64::<byteorder::LittleEndian>().unwrap();
                let buffer_size_in = rdr.read_u32::<byteorder::LittleEndian>().unwrap();

                let mut raw_data_buffer: Vec<u8> = Vec::with_capacity(read_buffer_size);
                buf_reader.read_to_end(&mut raw_data_buffer).unwrap();

                let compression_ratio: f32 = (buffer_size_in as f32) / (raw_data_buffer.len() as f32);
                println!("camera={}; frame={}-{}-{}; time={}; buffer_size={}; Got buffer: {} ({}X)", cameraid_in, exptid_in, burstid_in, frameid_in, timecode_in, buffer_size_in, raw_data_buffer.len(), compression_ratio);

                let new_frame = Frame {
                    cameraid: cameraid_in as u8,
                    frameid: frameid_in as u32,
                    timecode: timecode_in as u64,
                    data: raw_data_buffer // (1..10485760).map(|_x| 0 as u8).collect()
                };

                new_senders[new_frame.cameraid as usize].send(new_frame).unwrap();
            }

            while !new_senders.is_empty() {
                new_senders.pop().unwrap();
            }

            println!("Server thread {} is dead.", threadnum);
        }));
    }
    drop(tcp_reciever);

    let mut io_handles = Vec::new();
    for cameraid in 0..config.camera_count {
        let new_receiver = frame_receiver_channels[cameraid as usize].clone();
        let new_expt_receiver = expt_receiver_channels[cameraid as usize].clone();
        let output_dir = config.output_dir.clone();
        let archive_name = config.archive_name.clone();
        io_handles.push(std::thread::spawn( move || {
            let mut file = std::fs::File::create(
                Config::archive_path(&output_dir, &archive_name, cameraid)
            ).unwrap();
            let mut zip = zip::ZipWriter::new(file);
            let zip_options = zip::write::FileOptions::default().compression_method(zip::CompressionMethod::Stored);

            loop {
                // need to check for update to frame settings
                // The outer loop reads all the requests. The code always uses the most
                // recent name.
                while !new_expt_receiver.is_empty() {
                    // Load new settings
                    match new_expt_receiver.try_recv() {
                        Ok(res) => {
                            println!("Thread {} starting new file {}", cameraid, res);

                            // close old file
                            zip.finish().unwrap();

                            // open new file
                            file = std::fs::File::create(
                                Config::archive_path(&output_dir, &res, cameraid)
                            ).unwrap();
                            zip = zip::ZipWriter::new(file);
                        },
                        Err(_) => {}
                    }
                }

                // check for new buffer
                let buffer = match new_receiver.try_recv() {
                    Ok(res) => res,
                    Err(crossbeam_channel::TryRecvError::Empty) => {
                        // No available connection, sleep and then loop back
                        std::thread::sleep(std::time::Duration::from_millis(1));
                        continue;
                    },
                    // Break loop because channel disconnected
                    Err(crossbeam_channel::TryRecvError::Disconnected) => break,
                };

                let fname_internal = format!("frame_{}_t{}.zstd", buffer.frameid, buffer.timecode);
                zip.start_file(fname_internal, zip_options).unwrap();
                zip.write(&buffer.data).unwrap();

                // Will this introduce a memory leak?
            }

            zip.finish().unwrap();

            println!("IO Thread {} is dead.", cameraid);
            
            //let now = std::time::Instant::now();
            //let elapsed = now.elapsed();
            //println!("Elapsed: {:.2?}", elapsed);
        }));
    }

    let ctl_handle = std::thread::spawn( move || {
        let listener = control_listener;

        for stream in listener.incoming() {
            let mut stream = stream.unwrap();
            let buf_reader = std::io::BufReader::new(&mut stream);
            let http_request: Vec<_> = buf_reader // from rust manual
                .lines()
                .map(|result| result.unwrap())
                .take_while(|line| !line.is_empty())
                .collect();
            //println!("Request: {:#?}", http_request);

            // If it is a GET request, parse
            if http_request[0].starts_with("GET") {
                // Split the request line into three part
                let request_parts: Vec<&str> = http_request[0].split_whitespace().collect();
                // Pull out the middle which is the file name
                let new_name = request_parts[1].replace("/", "");

                println!("New file name: {}", new_name);

                // Send new name to all io threads
                for i in expt_sender_channels.iter() {
                    i.send(new_name.clone()).unwrap();
                }

                // Send the 200 code to the client. This prevents an error message.
                let response = "HTTP/1.1 200 OK\r\n\r\n";
                stream.write_all(response.as_bytes()).unwrap();
            }            
        }
    });

    let main_server_handle = std::thread::spawn(move || {
        // Start listener for primary service
        let listener = frame_listener;
        //stream.set_linger(Some(Duration::from_secs(0))).expect("set_linger call failed");
        listener.set_nonblocking(true).expect("Cannot set non-blocking setting.");

        //for stream in listener.incoming() {
        //    tcp_sender.send(stream.unwrap()).unwrap();
        //}

        for stream in listener.incoming() {
            match stream {
                Ok(s) => {
                    s.set_nonblocking(false).expect("set_nonblocking call failed");
                    tcp_sender.send(s).unwrap();
                },
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    match kill_reciever.try_recv() {
                        Ok(_) => {
                            println!("Killing main server handle.");
                            break;
                        },
                        Err(_) => std::thread::sleep(std::time::Duration::from_millis(1)),
                    }
                }
                Err(e) => panic!("encountered IO error: {e}"),
            };
        }

        drop(tcp_sender);
    });

    ////////////////////////////////////
    // Below code waits until Ctrl-C is pressed

    let (ctrlc_sender, ctrlc_receiver) = crossbeam_channel::unbounded();
    ctrlc::set_handler(move || {
        ctrlc_sender.send(1).unwrap();
    }).expect("Failed to set exit handler.");

    loop {
        match ctrlc_receiver.try_recv() {
            Ok(_) => {
                println!("Ctrl-C signal recieved!");
                kill_sender.send(1).unwrap();
                break;
            },
            Err(_) => {}
        }

        std::thread::sleep(std::time::Duration::from_millis(1));
    }

    ////////////////////////////////////

    // Delete channels to prevent the iters from hanging
    drop(frame_receiver_channels);
    drop(frame_sender_channels);
    drop(expt_receiver_channels);
    
    main_server_handle.join().unwrap();
    //ctl_handle.join().unwrap(); // Not necessary for a correct shutdown

    while !io_handles.is_empty() {
        io_handles.pop().expect("Failed to read io_handle").join().unwrap();
    }

    while !server_handles.is_empty() {
        server_handles.pop().expect("Failed to read server_handles").join().unwrap();
    }
}