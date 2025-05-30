use std::io::prelude::*;

const READ_BUFFER_SIZE: usize = 10485760; // ~10MB

fn main() {
    let mut handles = Vec::new();
    for cameraid in 0..3 {
        for _thread in 0..2 {
            handles.push(std::thread::spawn( move || {
                for frameid in 0..500 {
                    let mut stream =  std::net::TcpStream::connect("127.0.0.1:5000")
                            .expect("Couldn't connect to the server...");
                    
                    let mut buf: Vec<u8> = Vec::new();
                    buf.push( cameraid as u8 );
                    buf.extend_from_slice( &(frameid as u64).to_le_bytes() );
                    stream.write_all(&buf).unwrap();

                    let buf: Vec<u8> = (0..READ_BUFFER_SIZE).map(|_| 0 as u8).collect();
                    stream.write_all(&buf).unwrap();
                }
            }));
        }
    }

    while !handles.is_empty() {
        handles.pop().expect("Failed to join thread").join().unwrap();
    }
}
