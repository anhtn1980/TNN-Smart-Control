# CHANGELOG — TNN Smart Control

Quy ước: mỗi file `.ino` có số version riêng (Semantic Versioning: MAJOR.MINOR.PATCH), chỉ tăng khi chính file đó được sửa. Mọi thay đổi của cả 2 firmware được ghi chung vào file này theo thứ tự thời gian để dễ theo dõi.

---

## Code_ArduinoMEGA2560_W5500_TCP_Max485_.ino

### [1.5.0] - 2026-06-20
Rollback `waitForBytes()` (thêm ở v1.1.0) — đây là nguyên nhân thật sự khiến mirror công tắc tường mất hoàn toàn (rollback `anyClientPending()` ở v1.4.0 không đủ, vì đó không phải nguyên nhân chính). Người dùng xác nhận công tắc tường hoạt động đúng trên code gốc (trước mọi thay đổi), và hoàn toàn không phản hồi sau khi nâng cấp — trong khi lệnh ghi relay qua UI vẫn hoạt động bình thường suốt các bản trước đó.

Phân tích: RS485 là bus bán song công, cần thời gian ổn định sau khi chuyển driver enable (DE_PIN) trước khi dữ liệu phản hồi đáng tin cậy. Bản gốc luôn đợi đủ `delay(60)` trước khi đọc. `waitForBytes()` ở v1.1.0 kiểm tra `RS485.available()` gần như ngay lập tức sau khi gửi lệnh đọc, nhiều khả năng bắt trúng nhiễu thoáng qua trên bus đúng lúc chuyển trạng thái driver — vì code không có xác thực CRC để loại dữ liệu rác, các byte "đủ số lượng" đó được xử lý như phản hồi thật nhưng giá trị sai/không ổn định. Vì input đọc cho mirror cần 2 lần đọc liên tiếp khớp nhau mới kích hoạt (debounce), dữ liệu không ổn định khiến điều kiện này không bao giờ thỏa — mirror coi như chết hẳn. Lệnh ghi relay (`rs485Send` đơn thuần, fire-and-forget, không đọc lại) không đi qua đường này nên không bị ảnh hưởng, giải thích vì sao UI vẫn hoạt động bình thường suốt thời gian mirror bị hỏng.
- `readIO()`: bỏ `waitForBytes()`, quay lại `delay(60)` cố định như bản gốc cho cả 2 lần đọc (input/output).
- Xóa macro `RS485_READ_TIMEOUT_MS` (không còn dùng).
- `IO_POLL_INTERVAL_MS` (gating tần suất gọi `readIO()` trong `loop()`, không liên quan tới cơ chế đọc dữ liệu) được giữ nguyên — không phải nguyên nhân gây lỗi.

### [1.4.0] - 2026-06-20
Rollback `anyClientPending()` (thêm ở v1.2.0) — sau khi test thực tế, thay đổi này làm **mất hẳn logic mirror công tắc vật lý gắn tường**. Nguyên nhân nghi ngờ: cơ chế gate "bỏ qua lượt polling I/O nếu đang có lệnh TCP chờ xử lý" có thể bị giữ đúng (luôn thấy có dữ liệu chờ) trong điều kiện thực tế, khiến vòng polling — nơi duy nhất phát hiện thay đổi từ input vật lý — gần như không bao giờ chạy. Vì lợi ích của thay đổi này (giảm độ trễ khi nhịp bấm trùng chu kỳ polling) chưa kịp xác nhận rõ ràng trong khi rủi ro gây mất hẳn 1 tính năng quan trọng là có thật, quyết định rollback hoàn toàn về logic polling thuần của v1.1.0 thay vì vá thêm.
- Xóa hàm `anyClientPending()`.
- `loop()`: bỏ điều kiện `!anyClientPending()` khỏi gate polling I/O, quay về đúng logic v1.1.0 (chỉ gate theo `IO_POLL_INTERVAL_MS`).

(Lưu ý: sau khi nạp v1.4.0, mirror công tắc tường vẫn chưa hoạt động — nguyên nhân thật sự được xác định ở v1.5.0 là `waitForBytes()`, không phải `anyClientPending()`. Mục này giữ lại để truy vết lịch sử thay đổi.)

### [1.3.0] - 2026-06-20
Sửa lỗi: bấm "TẮT TẤT CẢ" → đèn tắt thật nhưng nút trên UI vẫn hiện xanh (ON) vĩnh viễn cho tới khi bấm nút khác. Nguyên nhân: nhánh `set /system/all off` trong `execCommand()` không cập nhật lạc quan `outSnapshot` ngay lập tức như nhánh `/relay/.../state` vẫn làm — phản hồi trạng thái gửi về ESP32 ngay sau lệnh vẫn còn các bit ON cũ; lần đọc lại đúng 150ms sau đó chỉ sửa `outSnapshot` nội bộ trên MEGA mà không có cơ chế nào chủ động đẩy trạng thái mới đó qua lại cho client TCP.
- Thêm `outSnapshot = 0` và `mirrorBlockUntil` ngay khi xử lý `set /system/all off`, khớp với cách các lệnh relay riêng lẻ đã làm.

### [1.2.0] - 2026-06-20 (ROLLBACK — xem v1.4.0)
~~Giảm cộng hưởng giữa nhịp bấm nút dồn dập và chu kỳ polling I/O bằng cách nhường ưu tiên cho lệnh TCP, bỏ qua lượt polling nếu có lệnh đang chờ.~~ Đã rollback ở v1.4.0 do làm mất logic mirror công tắc vật lý. Giữ lại đoạn này để truy vết lịch sử thay đổi.

### [1.1.0] - 2026-06-18
Cải thiện độ nhạy xử lý lệnh relay từ mạng (khắc phục hiện tượng phải bấm nhiều lần mới thấy relay phản hồi):
- `readIO()`: thay `delay(60)` cố định bằng `waitForBytes()` — chờ chủ động và thoát ngay khi RS485 có đủ byte phản hồi (vẫn giữ trần an toàn 60ms qua `RS485_READ_TIMEOUT_MS`), giảm đáng kể thời gian block mỗi lần đọc I/O trong điều kiện bình thường.
- `loop()`: thêm `IO_POLL_INTERVAL_MS` (120ms) để giãn nhịp đọc mirror I/O thay vì gọi `readIO()` ngay ở mọi vòng `loop()` rảnh — nhường chu kỳ CPU cho `acceptClients()`/`handleTCP()` xử lý lệnh relay từ ESP32 nhanh hơn, không bị kẹt phía sau vòng polling mirror.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động để xác nhận board đang chạy đúng bản nào.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Toàn bộ logic giữ nguyên như code gốc: TCP server 4 client, điều khiển relay qua RS485 (16 kênh), mirror input vật lý, scene switch, sync trạng thái sau lệnh.

---

## Code_ESP32_Web_UI_TCP_client_.ino

### [1.3.0] - 2026-06-20
Rollback phần block đồng bộ của v1.2.0 — sau khi test thực tế, v1.2.0 khiến độ nhạy quay lại y như bản gốc (5-8 lần bấm), TỆ HƠN so với v1.1.0 (3-5 lần). Nguyên nhân: route `/cmd` ở v1.2.0 chờ đồng bộ (block tối đa 150ms) để lấy trạng thái thật từ MEGA trước khi trả lời — nhưng MEGA có vòng polling I/O định kỳ (~120ms, phục vụ mirror công tắc vật lý) khiến nó không đọc kịp lệnh TCP mới trong lúc đang polling. Vì chu kỳ polling đó gần trùng với nhịp bấm nút dồn dập, gần như mọi lần bấm đều rơi đúng lúc MEGA bận, khiến ESP32 phải chờ sát ngưỡng 150ms — lộ thẳng độ trễ xử lý của MEGA ra cho trình duyệt, thay vì giấu nó đi như trước.
- Xóa `waitMegaStatus()` và bỏ việc chờ đồng bộ trong route `/cmd` — trả lời ngay lập tức ("OK") như trước v1.2.0.
- JS (`/mega`): khôi phục cơ chế cũ — sau khi gửi `/cmd`, gọi `poll()` không-block (`setTimeout(poll, 80)`) để xác nhận trạng thái thật sớm mà không chặn vòng lặp chính của ESP32. `setInterval(poll, 2000)` vẫn giữ nguyên cho đồng bộ định kỳ.
- Đi kèm với bản này là MEGA v1.2.0 (xem mục tương ứng) — phần sửa thực sự giải quyết gốc rễ độ trễ nằm ở phía MEGA: ưu tiên xử lý lệnh TCP hơn vòng polling mirror I/O.

### [1.2.0] - 2026-06-18 (ROLLBACK — xem v1.3.0)
~~Giảm thêm độ trễ mỗi lần bấm nút relay bằng cách gộp `/cmd` + `/status` thành 1 round-trip có chờ đồng bộ.~~ Đã rollback do gây regression (xem giải thích ở v1.3.0). Giữ lại đoạn này để truy vết lịch sử thay đổi.

### [1.1.0] - 2026-06-18
Tránh mất/lỗi request khi gói TCP bị chia nhỏ:
- `handleWebRequest()`: thêm vòng chờ có timeout (`REQUEST_TIMEOUT_MS` = 500ms) để đọc đủ header HTTP (`\r\n\r\n`) trước khi parse, thay vì bỏ cuộc ngay khi `client.available()` tạm thời về 0 — trước đây có thể khiến request bị đọc cụt, không khớp route nào, lệnh bị âm thầm bỏ qua mà không có phản hồi lỗi rõ ràng.
- Thêm `FW_VERSION` và in ra Serial Monitor lúc khởi động.

### [1.0.0] - 2026-06-18
Baseline ban đầu (chốt mốc trước khi cải tiến độ nhạy nút relay). Web UI menu/mega/amx, API `/set`, `/cmd`, `/status`, `/api/status`, TCP client tới MEGA với auto-reconnect mỗi 3s.
