# CHANGELOG — TNN Smart Control

Quy ước: mỗi file `.ino` có số version riêng (Semantic Versioning: MAJOR.MINOR.PATCH), chỉ tăng khi chính file đó được sửa. Mọi thay đổi của cả 2 firmware được ghi chung vào file này theo thứ tự thời gian để dễ theo dõi.

---

## Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino

### [1.1.0] - 2026-06-18
Cải thiện độ nhạy xử lý lệnh relay từ mạng (khắc phục hiện tượng phải bấm nhiều lần mới thấy relay phản hồi):
- `readIO()`: thay `delay(60)` cố định bằng `waitForBytes()` — chờ chủ động và thoát ngay khi RS485 có đủ byte phản hồi (vẫn giữ trần an toàn 60ms qua `RS485_READ_TIMEOUT_MS`), giảm đáng kể thời gian block mỗi lần đọc I/O trong điều kiện bình thường.
- `loop()`: thêm `IO_POLL_INTERVAL_MS` (120ms) để giãn nhịp đọc mirror I/O thay vì gọi `readIO()` ngay ở mọi vòng `loop()` rảnh — nhường chu kỳ CPU cho `acceptClients()`/`handleTCP()` xử lý lệnh relay từ ESP32 nhanh hơn, không bị kẹt phía sau vòng polling mirror.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động để xác nhận board đang chạy đúng bản nào.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Toàn bộ logic giữ nguyên như code gốc: TCP server 4 client, điều khiển relay qua RS485 (16 kênh), mirror input vật lý, scene switch, sync trạng thái sau lệnh.

---

## Code_ESP32_Web_UI_TCP_client_.ino

### [1.1.0] - 2026-06-18
Tránh mất/lỗi request khi gói TCP bị chia nhỏ:
- `handleWebRequest()`: thêm vòng chờ có timeout (`REQUEST_TIMEOUT_MS` = 500ms) để đọc đủ header HTTP (`\r\n\r\n`) trước khi parse, thay vì bỏ cuộc ngay khi `client.available()` tạm thời về 0 — trước đây có thể khiến request bị đọc cụt, không khớp route nào, lệnh bị âm thầm bỏ qua mà không có phản hồi lỗi rõ ràng.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Web UI menu/mega/amx, API `/set`, `/cmd`, `/status`, `/api/status`, TCP client tới MEGA với auto-reconnect mỗi 3s.
