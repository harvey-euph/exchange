# Continuous Deployment (CD) Setup Guide

This guide explains how to set up automatic deployment for the Exchange project on push to the `main` branch. When you push to GitHub, a GitHub Actions workflow will SSH into your GCP VM, pull the latest code, build it, and restart the exchange.

---

## Step 1: Configure Passwordless Sudo on GCP VM

The `kill-all` and `run-services` scripts use `sudo` to set real-time thread priorities (`chrt`), nice levels (`nice`), and clean up shared memory segments. In a non-interactive CD pipeline, the sudo prompt will hang. We must allow passwordless sudo.

Log in to your GCP VM and choose **one** of the following options:

### Option A: Fully Passwordless Sudo (Recommended for Dev VM)
Open the sudoers configuration:
```bash
sudo visudo
```
Add the following line at the end of the file (replace `harvey` with your actual VM username):
```sudoers
harvey ALL=(ALL) NOPASSWD: ALL
```

### Option B: Restrictive Passwordless Sudo
If you prefer restricting passwordless sudo, run:
```bash
sudo visudo -f /etc/sudoers.d/exchange
```
Add the following line (which includes `/usr/bin/true` so the scripts can check for passwordless status):
```sudoers
harvey ALL=(ALL) NOPASSWD: /usr/bin/nice, /usr/bin/chrt, /usr/bin/pkill, /usr/bin/chmod, /usr/bin/rm, /usr/bin/true
```

---

## Step 2: Generate and Configure SSH Keys

GitHub Actions needs an SSH private key to connect to your GCP VM.

1. **Generate a new SSH key pair** on your local machine or the GCP VM:
   ```bash
   ssh-keygen -t ed25519 -C "github-actions-exchange" -f ~/.ssh/id_exchange_cd -N ""
   ```
2. **Authorize the public key** on your GCP VM:
   ```bash
   cat ~/.ssh/id_exchange_cd.pub >> ~/.ssh/authorized_keys
   chmod 600 ~/.ssh/authorized_keys
   ```
3. **Copy the private key** content from `~/.ssh/id_exchange_cd`:
   ```bash
   cat ~/.ssh/id_exchange_cd
   ```

---

## Step 3: Configure GitHub Secrets

Go to your GitHub repository: **Settings** -> **Secrets and variables** -> **Actions** -> **New repository secret**.

Add the following three secrets:
- `GCP_SSH_PRIVATE_KEY`: Paste the complete private key content (including `-----BEGIN OPENSSH PRIVATE KEY-----` and `-----END OPENSSH PRIVATE KEY-----`).
- `GCP_VM_HOST`: The external IP address of your GCP VM.
- `GCP_VM_USER`: The username on the GCP VM (e.g., `harvey`).

---

## Step 4: Add GitHub Actions CD Workflow

Create `.github/workflows/deploy.yml` in your repository. Here is the workflow configuration:

```yaml
name: Continuous Deployment

on:
  push:
    branches: [ main ]

jobs:
  test:
    name: Run Tests First
    uses: ./.github/workflows/orderbook-ci.yml

  deploy:
    name: Deploy to GCP VM
    needs: test
    runs-on: ubuntu-latest
    steps:
      - name: SSH and Deploy
        uses: appleboy/ssh-action@v1.0.3
        with:
          host: ${{ secrets.GCP_VM_HOST }}
          username: ${{ secrets.GCP_VM_USER }}
          key: ${{ secrets.GCP_SSH_PRIVATE_KEY }}
          port: 22
          script: |
            cd /home/${{ secrets.GCP_VM_USER }}/exchange
            git fetch origin main
            git reset --hard origin/main
            make -j$(nproc)
            ./kill-all
            ./run-all
```

---

## Step 5: Test the Pipeline

1. Commit and push the `.github/workflows/deploy.yml` and this guide.
2. Go to the **Actions** tab of your GitHub repository.
3. You will see the pipeline run in two phases:
   - **Run Tests First**: Runs your CI tests to verify compilation and unit tests.
   - **Deploy to GCP VM**: Log in via SSH, pulls code, compiles, and restarts.
4. Verify the running sessions on the GCP VM using `tmux ls`.
