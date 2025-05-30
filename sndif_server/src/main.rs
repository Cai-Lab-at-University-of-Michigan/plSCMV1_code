use std::io::Write;
use std::io::Read;

use std::io::prelude::*;

use byteorder::ReadBytesExt;

const SERVER_THREADS: u32 = 10;
const CAMERA_COUNT: u8 = 3;
const READ_BUFFER_SIZE: usize = 10485760; // ~10MB

struct Frame {
    cameraid: u8,
    frameid: u32,
    timecode: u64,
    data: Vec<u8>
}

fn main() {
    let (kill_sender, kill_reciever) = crossbeam_channel::unbounded();
    let (tcp_sender, tcp_reciever) = crossbeam_channel::unbounded();
    let mut frame_sender_channels = Vec::new();
    let mut frame_receiver_channels = Vec::new();
    let mut expt_sender_channels: Vec<crossbeam_channel::Sender<String>> = Vec::new();
    let mut expt_receiver_channels = Vec::new();

    for _ in 0..CAMERA_COUNT {
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
    for threadnum in 0..SERVER_THREADS {
        let mut new_senders: Vec<crossbeam_channel::Sender<Frame>> = frame_sender_channels.iter().map(|x| (*x).clone()).collect();
        let tcp_reciever = tcp_reciever.clone();
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

                const HEADER_BUFFER_SIZE: usize = 32; // hmmmm // 1 + 4 + 4 + 4 + 8 + 4; 
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

                let mut raw_data_buffer: Vec<u8> = Vec::with_capacity(READ_BUFFER_SIZE);
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
    for cameraid in 0..CAMERA_COUNT {
        let new_receiver = frame_receiver_channels[cameraid as usize].clone();
        let new_expt_receiver = expt_receiver_channels[cameraid as usize].clone();
        io_handles.push(std::thread::spawn( move || {
            let mut file = std::fs::File::create(format!("DEFAULT_ch{}.zip", cameraid)).unwrap();
            let mut zip = zip::ZipWriter::new(file);
            let zip_options = zip::write::FileOptions::default().compression_method(zip::CompressionMethod::Stored);

            loop {
                // need to check for update to frame settings
                // Outer loop ensures that multiple requests are handled correctly; always want "latest"
                while !new_expt_receiver.is_empty() {
                    // Load new settings
                    match new_expt_receiver.try_recv() {
                        Ok(res) => {
                            println!("Thread {} starting new file {}", cameraid, res);

                            // close old file
                            zip.finish().unwrap();

                            // open new file
                            file = std::fs::File::create(format!("{}_ch{}.zip", res, cameraid)).unwrap();
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
        let listener = std::net::TcpListener::bind("0.0.0.0:8090").unwrap();

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

                // Write 200 code to client to prevent weird errors on HTTP
                let response = "HTTP/1.1 200 OK\r\n\r\n";
                stream.write_all(response.as_bytes()).unwrap();
            }            
        }
    });

    let main_server_handle = std::thread::spawn(move || {
        // Start listener for primary service
        let listener = std::net::TcpListener::bind("0.0.0.0:8080").unwrap();
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
    //ctl_handle.join().unwrap(); // ignore because it doesn't really matter

    while !io_handles.is_empty() {
        io_handles.pop().expect("Failed to read io_handle").join().unwrap();
    }

    while !server_handles.is_empty() {
        server_handles.pop().expect("Failed to read server_handles").join().unwrap();
    }
}