#!/bin/bash

# Default parameters
DB_NAME="exchange"
DB_USER="postgres"

# Load .env file if it exists
if [ -f .env ]; then
  # Export variables from .env to make them available, ignoring comments
  export $(grep -v '^#' .env | xargs)
fi


echo "Clearing data from PostgreSQL..."

# Execute TRUNCATE on clients which will CASCADE to positions, open_orders, and pending_responses.
sudo -u "$DB_USER" psql -d "$DB_NAME" -c "TRUNCATE TABLE clients RESTART IDENTITY CASCADE;"

echo "Database cleared successfully!"
