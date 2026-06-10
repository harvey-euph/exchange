# Nginx Configuration for Exchange Project

本文檔記錄了將 `web` 前端通過 Nginx 暴露到外部網路的設置步驟。

## 📁 檔案說明
- [exchange.conf](file:///home/andy16384/exchange/nginx/exchange.conf): Nginx 伺服器配置檔範本。
- [setup_nginx.sh](file:///home/andy16384/exchange/nginx/setup_nginx.sh): 自動化安裝與設定腳本。

## 🚀 快速開始 (自動化腳本)
我們提供了一個自動化腳本來處理所有事情（安裝 Nginx/Node.js/NPM、編譯前端、套用 Nginx 設定、解決權限問題並啟動服務）：

請在終端機執行：
```bash
sudo ./setup_nginx.sh
```

## 🛠️ 腳本執行內容與手動操作參考
如果您想要了解細節或進行手動調整，該腳本依序執行了以下操作：

1. **安裝依賴**
   ```bash
   sudo apt-get update
   sudo apt-get install -y nginx nodejs npm
   ```

2. **編譯前端**
   在 `web` 目錄下：
   ```bash
   npm install
   npm run build
   ```

3. **套用 Nginx 設定**
   - 將 `exchange.conf` 中的 `__FRONTEND_DIST_PATH__` 替換為實際編譯後的 `dist` 資料夾絕對路徑。
   - 複製設定檔至 `/etc/nginx/sites-available/exchange`。
   - 啟用該設定並移除預設的 `default` 設定以避免衝突：
     ```bash
     sudo rm -f /etc/nginx/sites-enabled/default
     sudo ln -sf /etc/nginx/sites-available/exchange /etc/nginx/sites-enabled/exchange
     ```

4. **權限設定 (解決 500 Internal Error)**
   為了讓 Nginx 伺服器能讀取家目錄 `/home` 下的靜態檔案，必須將 Nginx 的執行使用者 `www-data` 加入當前使用者的群組中：
   ```bash
   sudo usermod -aG <您的使用者群組> www-data
   ```

5. **套用並重啟**
   ```bash
   sudo nginx -t
   sudo systemctl restart nginx
   ```

## 📝 維護建議
- **更新前端代碼**：若之後修改了前端代碼，請在 `web` 目錄下執行 `npm run build` 更新 `dist` 目錄即可。
- **防火牆設置**：確保 GCP 控制台已開放 **TCP 80** 端口。
- **日誌查看**：
  - 訪問日誌：`/var/log/nginx/access.log`
  - 錯誤日誌：`/var/log/nginx/error.log`
