#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Configuration (can be overridden via environment variables)
REMOTE_HOST="${REMOTE_HOST:-plowsof}"
SERVICE_NAME="${SERVICE_NAME:-sof_winserver1}"
LOCAL_FILE="${LOCAL_FILE:-$PROJECT_ROOT/build/gamex86.dll}"
REMOTE_DEST="${REMOTE_DEST:-winserver1/Base/}"

# If LOCAL_FILE was passed as a relative path and not found in current directory, check project root
if [[ ! -f "$LOCAL_FILE" && -f "$PROJECT_ROOT/$LOCAL_FILE" ]]; then
    LOCAL_FILE="$PROJECT_ROOT/$LOCAL_FILE"
fi

# 1. Ensure the local artifact exists before taking down the server
if [[ ! -f "$LOCAL_FILE" ]]; then
    echo "Error: Local artifact '$LOCAL_FILE' not found. Build the project before deploying." >&2
    exit 1
fi

SERVICE_STOPPED=false

# 2. Trap exit to ensure the server is restarted if the script fails midway
cleanup() {
    if [[ "$SERVICE_STOPPED" == true ]]; then
        echo "Warning: Process interrupted. Attempting to restart $SERVICE_NAME..." >&2
        ssh "$REMOTE_HOST" "sudo -S systemctl start $SERVICE_NAME" || true
    fi
}
trap cleanup EXIT

# 3. Stop service
echo "==> Stopping remote service: $SERVICE_NAME..."
ssh "$REMOTE_HOST" "sudo -S systemctl stop $SERVICE_NAME"
SERVICE_STOPPED=true

# 4. Upload updated artifact
echo "==> Uploading $LOCAL_FILE to $REMOTE_HOST:$REMOTE_DEST..."
scp "$LOCAL_FILE" "$REMOTE_HOST:$REMOTE_DEST"

# 5. Start service
echo "==> Starting remote service: $SERVICE_NAME..."
ssh "$REMOTE_HOST" "sudo -S systemctl start $SERVICE_NAME"
SERVICE_STOPPED=false

# 6. Verify service status
echo "==> Verifying service status..."
if ssh "$REMOTE_HOST" "sudo -S systemctl is-active --quiet $SERVICE_NAME"; then
    echo "==> Deployment successful: $SERVICE_NAME is running."
else
    echo "Error: $SERVICE_NAME failed to restart properly." >&2
    exit 1
fi