# RFC-IPC-001 — IPC, shared memory and system event semantics

**Статус:** DRAFT — observed current behavior plus target contract requirements.  
**База:** main @ bbc7f7ab948e72511aed113b69043add10ec8350.

## 1. Current implementation facts

- `kobj.c` implements named PORT and SHM kernel objects in a shared global name registry.
- Ports use bounded queues (`PORT_QUEUE=64`), messages up to `PORT_MSG_MAX=65536`, sender PID metadata and waiter wakeups.
- `syscall.c` exposes PORT_OPEN/SEND/RECV and SHM_OPEN/MAP/UNMAP. Process handles are integer indexes in a per-process handle table.
- `proc.c` retains/releases object references for handles and SHM mappings; process teardown releases handles and mappings.
- `futex.c` keys waiters by physical address so a shared word mapped by multiple processes can synchronize; wait checks value and queues atomically under bucket lock.
- `CAP_IPC` is a process-level bitmask gate for port and SHM syscalls.
- No general typed system Event Bus contract was identified. Local GUI/event queues are not automatically a global event bus.

## 2. Required target contract

### Endpoints and names
- endpoint identity must be unforgeable and separate from discovery name;
- name lookup is a rendezvous mechanism, not authority by itself;
- access control is applied to open, send, receive, inspect and destroy separately;
- names have explicit namespace and ownership policy.

### Messages
- typed/versioned schema or opaque byte payload with explicit version;
- maximum message size and queue capacity are negotiated/documented;
- sender identity is kernel-authenticated, never caller-supplied;
- send/receive semantics define copy, move, shared buffer and ownership;
- backpressure behavior: fail, block with timeout, or drop according to endpoint policy;
- cancellation, peer crash, endpoint close and queue drain are specified.

### Shared memory
- explicit create/map/unmap/close and reference lifecycle;
- read/write rights per mapping;
- mapping generation and revocation semantics;
- page ownership, zero-on-create, bounds and alignment;
- synchronization is separate from memory visibility (futex/atomics contract);
- cross-ISA memory ordering must be documented.

### Event Bus
- build as a typed service on IPC rather than bypassing IPC;
- versioned event schema, publisher/subscriber capabilities, ordering guarantees, bounded queues and backpressure;
- events are hints/notifications, not a substitute for authoritative state reads.

## 3. Important semantic issue to resolve

Current `SYS_PORT_RECV` removes a message from the queue before copying its bytes into the user buffer. If `copy_to_user` then fails (e.g. concurrent unmap), the message is lost and the syscall returns `KE_ARG`. This behavior is explicitly commented in the source. Decide whether this at-most-once/loss-on-invalid-buffer behavior is acceptable or whether receive should pin/validate destination memory before dequeue or use a retry-safe message token. Any change must address races and avoid holding port spinlocks while touching user memory.

## 4. Tests

- unauthorized open/send/receive;
- queue full and backpressure;
- sender exit and receiver exit while blocked;
- timeout, cancellation and spurious wake;
- malformed name and maximum length boundaries;
- SHM zeroing, mapping rights, unmap, object lifetime and process teardown;
- futex lost-wakeup, shared physical key and cross-ISA ordering;
- peer crash and message integrity;
- receive destination unmapped concurrently;
- event subscriber crash, slow subscriber and queue overflow.
