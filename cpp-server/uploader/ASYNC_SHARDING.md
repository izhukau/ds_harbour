# L5. Async Payments + Sharded RDS

Turns the synchronous payment upload from homework 1 into an async service backed
by PostgreSQL, with the `payments` table sharded across more than one database
instance.

## Flow

1. Client sends a bulk request with many payments to `POST /requests`.
2. The server writes every payment to the database and returns a `requestId` right
   away (`202 Accepted`) — it does not wait for the remote system.
3. Client polls `GET /requests/{id}` to see progress.
4. A background worker picks up the request, and for each payment creates an entry
   in the remote system (the homework-1 central app) and updates its status. When
   all payments of a request are processed, the request becomes `DONE`.

## Sharding

- Two tables live on every database instance: `requests` and `payments`.
- A payment goes to a shard chosen by `fnv1a(transactionId) % shardCount`, so the
  `payments` table is spread across all instances.
- A request row goes to `fnv1a(requestId) % shardCount`.
- The status query reads the request row from its shard and scatter-gathers payment
  counts from all shards.
- **Number of shards is static and configurable**: it equals the number of
  connection strings passed on the command line. No resharding.

## API

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/requests` | Body `{"payments":[{storeId,transactionId,coffeeType,price,currency,loyaltyCardId}, ...]}`. Returns `202 {"requestId": "..."}`. |
| `GET` | `/requests/{id}` | Returns `{requestId,status,total,done,failed,pending}`. `404` if unknown. |
| `GET` | `/health` | Liveness. |

Request status: `PENDING -> PROCESSING -> DONE`. Payment status: `PENDING -> DONE` or `FAILED`.

## Run it

```bash
cmake -S . -B build && cmake --build build -j

./scripts/pg_shards.sh up 2

java -jar cloud-0.0.1-SNAPSHOT.jar --server.port=8080 &

./build/async_server 9090 http://127.0.0.1:8080 \
  "host=localhost port=5441 dbname=payments user=postgres" \
  "host=localhost port=5442 dbname=payments user=postgres"
```

The number of connection strings = the number of shards. Add a third one (and
`./scripts/pg_shards.sh up 3`) to run with three shards.

`scripts/pg_shards.sh {up|down|destroy|conn} [N]` starts/stops N local PostgreSQL
instances on ports `5441, 5442, ...`. Schema (`sql/schema.sql`) is also created
automatically by the server on startup.

## Tested

```bash
ctest --test-dir build --output-on-failure
```

End-to-end with 2 real Postgres instances + the remote app:

```bash
curl -X POST http://127.0.0.1:9090/requests -H 'Content-Type: application/json' \
  -d '{"payments":[{"storeId":"s1","transactionId":"t1","coffeeType":"LATTE","price":3.5,"currency":"EUR","loyaltyCardId":"c1"}]}'
# {"accepted":1,"requestId":"...."}

curl http://127.0.0.1:9090/requests/<id>
# {"done":1,"failed":0,"pending":0,"requestId":"...","status":"DONE","total":1}
```

Verified that the 28 sample payments split evenly across shards (14/14 on two
shards), and across three shards too, and that each payment lands in the remote
system with its `remote_id` stored back.
