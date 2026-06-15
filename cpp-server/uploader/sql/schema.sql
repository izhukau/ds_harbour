CREATE TABLE IF NOT EXISTS requests (
    id UUID PRIMARY KEY,
    status TEXT NOT NULL,
    total INT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS payments (
    transaction_id TEXT PRIMARY KEY,
    request_id UUID NOT NULL,
    store_id TEXT NOT NULL,
    coffee_type TEXT NOT NULL,
    price NUMERIC(12, 2) NOT NULL,
    currency TEXT NOT NULL,
    loyalty_card_id TEXT,
    status TEXT NOT NULL,
    remote_id TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS payments_request_id_idx ON payments (request_id);
