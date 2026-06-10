# PostgreSQL Operation Manual for Exchange

This manual documents the transition from an in-memory client database to PostgreSQL, including system installation, database creation, schema setup, code architecture, and troubleshooting procedures. Follow this guide to reproduce the database environment from scratch.

---

## 1. Prerequisites & Installation

To run the PostgreSQL database and build the C++ PostgreSQL client library (`libpqxx`), you need to install the database server, development headers, and the C++ driver.

### Ubuntu/Debian Installation

Run the following command to install the required packages:

```bash
sudo apt-get update
sudo apt-get install -y postgresql libpq-dev libpqxx-dev
```

- **`postgresql`**: The core PostgreSQL database server and client utilities (`psql`).
- **`libpq-dev`**: PostgreSQL C client library headers (required by libpqxx).
- **`libpqxx-dev`**: PostgreSQL C++ client library wrapper (`pqxx` namespace).

### Verify Service Status

Ensure that the PostgreSQL service is active and running:

```bash
systemctl status postgresql
```

If it is not running, start it:

```bash
sudo systemctl start postgresql
```

---

## 2. Database & User Creation

The exchange system uses **PostgreSQL Peer Authentication** for local connections, which maps the OS user directly to the PostgreSQL user. This avoids hardcoding plain-text passwords.

### Step-by-Step Setup

1. **Switch to the `postgres` system user** to execute administrative commands:
   ```bash
   sudo -i -u postgres
   ```

2. **Create the PostgreSQL role/user** matching your OS username. Grant it superuser privileges so it can manage databases and schemas:
   ```bash
   createuser --superuser $(whoami)
   ```

3. **Create the target database** named `exchange` owned by your user:
   ```bash
   createdb exchange -O $(whoami)
   ```

4. **Exit the `postgres` shell** to return to your normal user context:
   ```bash
   exit
   ```

5. **Test the connection** using `psql` directly (without password):
   ```bash
   psql -d exchange
   ```
   If successful, you will see the `exchange=>` prompt.

---

## 3. Schema & Data Initialization

The database schema is defined in [docs/schema.sql](https://github.com/harvey-euph/exchange/docs/schema.sql). It defines tables for symbols, clients, positions, active open orders, and offline pending responses.

### Initialize/Re-apply Schema

To create the tables and load initial symbols (BTC, ETH, SOL) and clients (`client_1`, `client_2`), run the schema file:

```bash
psql -d exchange -f docs/schema.sql
```

### Table Definitions Reference

- **`symbols`**: Defines trading instrument specifications (scaling, step sizes, price boundaries).
  - `p_exp`: Price exponent (e.g., `-2` for `10^-2` / `0.01` scale).
  - `min_step_raw`: Minimum tick size represented as an integer.
  - `min_price_raw` & `max_price_raw`: Safe trading boundaries as raw integers.
- **`clients`**: System client/trader registry.
- **`positions`**: Asset balances. Cash is tracked as `symbol_id = 0`, while symbols are `symbol_id > 0`.
- **`open_orders`**: Current active orders inside the Order Book. Used to rebuild the order book state on recovery.
- **`pending_responses`**: Serialized FlatBuffers execution reports stored for offline/disconnected clients, popped once they reconnect.

---

## 4. C++ Integration Architecture

We use a modular, thread-safe database abstraction layers to interact with PostgreSQL.

### 4.1 Connection Utility ([DbUtil.hpp](file:///home/$(whoami)/exchange/include/DbUtil.hpp))

Provides a centralized getter for database connections. It supports configuration via the `DATABASE_URL` environment variable and defaults to local peer auth.

```cpp
namespace Exchange {
namespace DbUtil {
    // Reads DATABASE_URL or defaults to "dbname=exchange host=127.0.0.1"
    std::string getConnectionString();
    
    // Allocates a unique_ptr to a new pqxx::connection
    std::unique_ptr<pqxx::connection> getDbConnection();
}
}
```

### 4.2 Database Adaptor Interface ([ClientDatabase.hpp](file:///home/$(whoami)/exchange/include/ClientDatabase.hpp))

An abstract base class `ClientDatabase` allows hot-swapping between `InMemoryClientDatabase` (for fast unit testing) and `PostgresClientDatabase` (for production/durability).

```cpp
class ClientDatabase {
public:
    virtual ~ClientDatabase() = default;
    virtual void addPendingResponse(uint32_t client_id, const uint8_t* data, size_t size) = 0;
    virtual std::vector<PendingResponse> popPendingResponses(uint32_t client_id) = 0;
    virtual int64_t getPosition(uint32_t client_id, uint32_t symbol_id) = 0;
    virtual void updatePosition(uint32_t client_id, uint32_t symbol_id, int64_t delta) = 0;
    virtual void addOrUpdateOpenOrder(uint32_t client_id, uint64_t order_id, const uint8_t* data, size_t size) = 0;
    virtual void removeOpenOrder(uint32_t client_id, uint64_t order_id) = 0;
};
```

### 4.3 Dynamic Client Registration

To prevent `foreign_key_violation` crashes when new HFT client agents or Web UI connections submit orders or request position updates with un-registered `client_id`s, `PostgresClientDatabase` employs a dynamic client upsert mechanism.

Before inserting records into `positions`, `open_orders`, or `pending_responses` (which have foreign key references to `clients`), the database layer dynamically registers the client into the `clients` table on-the-fly:

```sql
INSERT INTO clients (client_id, username) 
VALUES ($1, $2) 
ON CONFLICT (client_id) DO NOTHING;
```

This guarantees database integrity while allowing arbitrary client ids (e.g. from random noise traders or hashed Web UI client strings) to trade immediately without manual pre-registration.

---

## 5. Build System & ODR Debugging Notes

During the integration of `pqxx`, a severe ODR (One Definition Rule) double-destruction bug was discovered and fixed.

### The Double-Free Bug
If `PostgresClientDatabase` methods were fully defined inline inside headers or included in multiple translation units that link into Google Test binaries, the linker would duplicate template instantations and static registries from `libpqxx.so`. On exit, this triggered double-free errors:
```
double free or corruption (out)
Aborted (core dumped)
```

### The Fix
1. **Decouple Header and Source**: Keep `PostgresClientDatabase` declarations clean in `include/ClientDatabase.hpp` and move all implementation logic, including the direct `#include <pqxx/pqxx>`, exclusively into [src/PostgresClientDatabase.cpp](file:///home/$(whoami)/exchange/src/PostgresClientDatabase.cpp).
2. **Exclude Database Objects from Unit Tests**: In the [Makefile](file:///home/$(whoami)/exchange/Makefile), unit tests are built by filtering out the PostgreSQL/Database source object files:
   ```makefile
   $(BUILD_DIR)/tests/%: $(TEST_DIR)/%.cpp $(SRC_OBJECTS) $(FBS_GENERATED)
       $(CXX) $(CXXFLAGS) $(INCLUDES) $< $(filter-out $(BUILD_DIR)/PostgresClientDatabase.o $(BUILD_DIR)/ClientManager.o $(BUILD_DIR)/AlgoTradingClient.o,$(SRC_OBJECTS)) $(TEST_LDLIBS) -o $@
   ```
   This prevents Google Test binaries from pulling in the PostgreSQL translation unit, avoiding conflicts and ensuring all tests pass cleanly.

---

## 6. Verification and Execution

### Build the Services
Clean and compile the entire project (compiling services, agents, examples, and tests):

```bash
make clean
make -j$(nproc)
```

### Run Core Services
Launch the core exchange services (including `matching-engine`, `client-manager`, and `public-data`):

```bash
./run-services
```
*Note: `public-data` is dynamically discovered and launched by the script.*

To check active services running inside tmux, run:
```bash
tmux list-windows -t exchange
```

To stop all services and clean up resources:
```bash
./kill-all
```
