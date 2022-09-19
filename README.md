# FWD-PROXY

This is a test project implementing a very basic forward proxy.

```
[Client A] <-> [Proxy] <-> [Client B]
```

When 2 clients connect to the proxy server they are matched (when anonymous or their "secret" are the same) and, once the pairing is complete, each can send messages to the other via the proxy.

## Implementation details

### Server

The server has 3 threads:
1. **connection worker**: Accepts incoming connection requests.

2. **pending worker**: Processes the "handshake" for new connections and keeps track of pending ones that have completed the handshake successfully. When a client pair is matched, the clients are moved into the proxy thread via a "pairing" data-structure (uses mutex).  

3. **proxy worker**: Processes incoming messages and forwards them to the paired client.

All pending and current opened file descriptors for the client sockets are *polled* via a call to `epoll_wait(..)`.

#### Comments

- Use of a mutex to access/check the pairing store by both the *pending* and *proxy* thread kinda sucks. Passing paired clients file descriptors via a lock-less queue might yield better results as it won't be a blocking operation.

- When a paired client disconnects the other one is left hanging and the only recourse is to boot it out. Realistically it would be better to move it back to the pending store and connect it back if another client that mach connect before the timout occurs.

- In high traffic throughput situations it might be advantageous to create 1 proxy worker thread per paired client so that if many clients all send messages at the same time their forwarding operations won't be sequentially processed.

### Client

Nothing too crazy going on here. The point of it is to test the server. There is a buffered `send` so that even if the processing thread is occupied in fetching content from the socket buffer, it is still possible to queue up content to be sent.  It could be better implemented but, again, this is not the main focus here.

## Compiling and running

Linux only.

1. Clone the repository `git clone https://github.com/An7ar35/fwd-proxy.git`
2. Get in the directory with `cd fwd-proxy`
3. run `cmake .`
4. run `cmake --build .`
5. Done.

**Server:** `./fwd-proxy -m server`

**Client:** `./fwd-proxy -m client` (or `./fwd-proxy -m client -s secret` to use a "secret" - replace `secret` with whatever string you wish)

## License

AGPLv3