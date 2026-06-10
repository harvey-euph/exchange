-- PostgreSQL Database Schema Design for Exchange

CREATE TABLE IF NOT EXISTS symbols (
    symbol_id SERIAL PRIMARY KEY,
    name VARCHAR(32) NOT NULL UNIQUE,
    p_exp SMALLINT NOT NULL,
    min_step_raw BIGINT NOT NULL,
    min_price_raw BIGINT NOT NULL,
    max_price_raw BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS clients (
    client_id SERIAL PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS positions (
    client_id INTEGER REFERENCES clients(client_id) ON DELETE CASCADE,
    symbol_id INTEGER NOT NULL,            -- 0 represents USD/Cash, >0 represents asset symbol_id
    position BIGINT NOT NULL DEFAULT 0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (client_id, symbol_id)
);

CREATE TABLE IF NOT EXISTS open_orders (
    order_id BIGINT PRIMARY KEY,
    client_id INTEGER REFERENCES clients(client_id) ON DELETE CASCADE,
    symbol_id INTEGER REFERENCES symbols(symbol_id),
    side SMALLINT NOT NULL,                -- 0: Buy, 1: Sell
    price_mantissa BIGINT NOT NULL,
    qty BIGINT NOT NULL,
    visible_qty BIGINT NOT NULL DEFAULT 0,
    timestamp TIMESTAMP NOT NULL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS pending_responses (
    response_id BIGSERIAL PRIMARY KEY,
    client_id INTEGER REFERENCES clients(client_id) ON DELETE CASCADE,
    exec_id BIGINT NOT NULL,
    serialized_data BYTEA NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_pending_responses_client ON pending_responses(client_id);

-- Insert initial symbol data
INSERT INTO symbols (symbol_id, name, p_exp, min_step_raw, min_price_raw, max_price_raw) VALUES
(1, 'BTC', -2, 25, 3000000, 12000000),
(2, 'ETH', -2, 10, 150000, 600000),
(3, 'SOL', -3, 5, 5000, 500000)
ON CONFLICT (symbol_id) DO UPDATE SET
    name = EXCLUDED.name,
    p_exp = EXCLUDED.p_exp,
    min_step_raw = EXCLUDED.min_step_raw,
    min_price_raw = EXCLUDED.min_price_raw,
    max_price_raw = EXCLUDED.max_price_raw;

-- Also insert initial client data if needed (let's insert client 1 and 2 if they exist/are used)
INSERT INTO clients (client_id, username) VALUES
(1, 'client_1'),
(2, 'client_2')
ON CONFLICT (client_id) DO NOTHING;
