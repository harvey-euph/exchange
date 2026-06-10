#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Make sure the script is run with sudo/root privileges
if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run this script with sudo (e.g., sudo ./setup_nginx.sh)"
  exit 1
fi

# Determine the original user who invoked sudo
REAL_USER=${SUDO_USER:-$(whoami)}
REAL_GROUP=$(id -gn "$REAL_USER")

# Resolve absolute paths
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

if [ -f "$SCRIPT_DIR/../web/package.json" ]; then
  FRONTEND_DIR="$( cd "$SCRIPT_DIR/../web" &> /dev/null && pwd )"
elif [ -f "$SCRIPT_DIR/../package.json" ]; then
  FRONTEND_DIR="$( cd "$SCRIPT_DIR/.." &> /dev/null && pwd )"
else
  echo "Error: Could not locate frontend directory containing package.json."
  echo "Looked in:"
  echo "  - $SCRIPT_DIR/../web"
  echo "  - $SCRIPT_DIR/.."
  exit 1
fi

DIST_DIR="$FRONTEND_DIR/dist"

echo "=== 1. Installing System Packages (Nginx, Node.js, NPM) ==="
apt-get update
apt-get install -y nginx nodejs npm

echo "=== 2. Building Frontend Application ==="
# Run npm commands as the real user so files are owned by them, not root
sudo -u "$REAL_USER" bash -c "cd '$FRONTEND_DIR' && npm install && npm run build"

echo "=== 3. Creating Nginx Configuration ==="
# Replace the placeholder in the template with the actual absolute path to dist/
SED_EXPR="s|__FRONTEND_DIST_PATH__|$DIST_DIR|g"
sed "$SED_EXPR" "$SCRIPT_DIR/exchange.conf" > /etc/nginx/sites-available/exchange

echo "=== 4. Enabling Configuration and Disabling Default Site ==="
rm -f /etc/nginx/sites-enabled/default
ln -sf /etc/nginx/sites-available/exchange /etc/nginx/sites-enabled/exchange

echo "=== 5. Granting Nginx Access Permissions ==="
# Add Nginx user (www-data) to the original user's primary group so it can traverse home directory
usermod -aG "$REAL_GROUP" www-data

echo "=== 6. Validating Nginx Configuration and Restarting ==="
nginx -t
systemctl restart nginx

echo "=== Setup Completed Successfully! ==="
echo "The exchange frontend has been successfully configured and started on Nginx (Port 80)."
