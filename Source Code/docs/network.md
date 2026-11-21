# Bounded loopback IPv4

Implemented in `src32/net.cpp`: a NetDevice transmit/poll/statistics interface, a bounded queue of complete IPv4 packets, IPv4 header/checksum/fragment/bounds validation, ICMP echo, connected and addressed UDP, and a small TCP state machine. Socket handles are typed process-local objects and are reclaimed when processes exit. No device DMA or kernel address is exposed to applications.

TCP implements SYN/SYN-ACK/ACK, accept, ordered byte-stream receive, duplicate suppression, checksums including pseudoheader, 512-byte MSS, a 1024-byte receive window, bounded send/receive buffers, stop-and-wait transmission, window reopening, fixed RTO with five retries, FIN and half-close handling. A sender's pending data is acknowledged before FIN is emitted.

User ABI: socket, bind, listen, connect, accept, send/recv, sendto/recvfrom, close, readiness polling, single-descriptor timed select and statistics. Calls are nonblocking (S_AGAIN); user runtime implements timed waits with Yield. Network access defaults to S_DENIED. Only the explicitly launched trusted boot network test receives CAP_NETWORK in this release. No external network interface is configured or exposed.

`user/netlib.cpp` implements bounded DNS A query/response parsing and an HTTP/1.0 GET helper using socket APIs. `tests/guest/nettest.cpp` runs a real user-thread DNS responder and HTTP server against these clients. DNS compression pointers and record sizes are checked; the resolver does not implement recursive resolution or DNSSEC. HTTP has bounded response storage and no TLS, redirects, chunked transfer or downloads/execution. The endpoint is loopback only.

Run `regression.ps1 -Suite network` from the repository root. It launches `nettest` in ShellBridge. The guest exercises loopback sockets, retransmission, DNS and HTTP. QEMU uses `-nic none`.

Primary specifications used as references, not imported code: [IPv4 RFC 791](https://www.rfc-editor.org/rfc/rfc791), [ICMP RFC 792](https://www.rfc-editor.org/rfc/rfc792), [UDP RFC 768](https://www.rfc-editor.org/rfc/rfc768), [TCP RFC 9293](https://www.rfc-editor.org/rfc/rfc9293), [DNS RFC 1035](https://www.rfc-editor.org/rfc/rfc1035).
