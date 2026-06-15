# Redirect Load Balancer (L2)

A redirect load balancer that distributes load between instances of the
payments app (homework 1) using **HTTP 302**: the client sends a request to
the balancer, gets `302 Found` with a `Location` header pointing at a concrete
backend instance, and re-sends the request there.

## Build & Run

```bash
cmake -S . -B build && cmake --build build -j

# start two app instances (Spring Boot) on different ports, then:
./build/load_balancer 8090 http://127.0.0.1:8080 http://127.0.0.1:8081
```

The uploader (`./build/app`) can be pointed at the balancer — its HTTP client
follows redirects (`set_follow_location(true)`), and cpp-httplib re-sends a
POST with the same body on 302.

```bash
curl -i 'http://127.0.0.1:8090/api/v1/payments?storeId=store-1'
# HTTP/1.1 302 Found
# Location: http://127.0.0.1:8080/api/v1/payments?storeId=store-1

curl -s http://127.0.0.1:8090/lb/status
# {"backends":[{"url":"http://127.0.0.1:8080","healthy":true,"redirects":3}, ...]}
```

## Design Decisions

### How to get the list of available services

**Static configuration via command-line arguments.** Backend URLs are passed
at startup. This is the simplest reliable option for a fixed set of instances.

Alternatives for a dynamic fleet: a service registry (Consul, etcd, Eureka)
where instances register themselves and the balancer subscribes to changes, or
DNS-based discovery (one name, many A records). Static config was chosen
because instances here are started manually and their addresses are known.

### How to do health checks

**Active periodic probing.** A background thread polls every backend every
3 seconds with `GET /api/v1/payments?storeId=lb-health-check` (the app has no
actuator, so we probe the real API). Rules:

- any HTTP response with status `< 500` → instance is **UP** (even 4xx means
  the server is alive and serving);
- `5xx`, connection refused, or a 2-second timeout → instance is **DOWN**.

A DOWN instance stops receiving redirects but keeps being probed, so it
returns to rotation automatically once it answers again. If *all* backends are
down, the balancer answers `503 {"error":"no healthy backends"}`.

### What algorithm to use

**Round-robin over the healthy subset.** Each request is redirected to the
next healthy backend in a circular order (`BackendPool::pickNext()`).

Why round-robin: the app instances are identical, requests are short and
uniform (one payment per POST), so an even distribution is exactly what we
want — no need for weights or least-connections. Per-backend redirect counters
are exposed at `/lb/status` to verify the distribution is even.

### Why 302 specifically

Per the assignment. Note that some clients rewrite POST→GET on 302; cpp-httplib
(used by the uploader) preserves the method and body, so the full
client → LB → backend flow works. `307 Temporary Redirect` would be the
strictly-correct status for method-preserving redirects in the general case.

## Endpoints

| Path | Behavior |
|------|----------|
| `/lb/status` | JSON with health and redirect count per backend |
| anything else (GET/POST/PUT/PATCH/DELETE) | `302` to the next healthy backend, path + query preserved |

## Tests

`tests/test_lb.cpp` (doctest) covers `BackendPool`: round-robin order,
skipping unhealthy backends, all-down → no backend, recovery, and redirect
counting.

```bash
ctest --test-dir build --output-on-failure
```
