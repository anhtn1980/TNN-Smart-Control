# GHI CHÚ CÁC LỆNH GIT VÀ HƯỚNG DẪN CÀI ĐẶT

## 1. Hướng dẫn tải và cài đặt Git cho PC (Windows)
1. Truy cập trang web chính thức: https://git-scm.com
2. Nhấn vào nút "Download for Windows".
3. Chọn bản "64-bit Git for Windows Setup" để tải file .exe về máy.
https://github.com/git-for-windows/git/releases/download/v2.54.0.windows.1/Git-2.54.0-64-bit.exe
4. Mở file .exe vừa tải, nhấn "Next" liên tục theo mặc định và chọn "Install".
5. Kiểm tra cài đặt thành công: Mở CMD/PowerShell, gõ lệnh: `git --version`

## 2. Cấu hình ban đầu (Chỉ làm 1 lần duy nhất sau khi cài Git)
```bash
git config --global user.name "Tên Của Bạn"
git config --global user.email "email_cua_ban@example.com"
```

## 3. Quy trình đưa dự án mới lên GitHub (Lần đầu tiên)
```bash
git init                              # Khởi tạo Git cho thư mục hiện tại
git add .                             # Thêm toàn bộ file vào vùng chờ (Staging Area)
git commit -m "Initial commit"        # Tạo điểm lưu trữ đầu tiên
git branch -M main                    # Đổi tên nhánh chính thành 'main'
git remote add origin <URL_Repository> # Liên kết thư mục máy tính với GitHub
git push -u origin main               # Đẩy toàn bộ code lên GitHub
```

## 4. Quy trình cập nhật code lên GitHub (Từ lần thứ 2 trở đi)
```bash
git add .                             # Thêm các thay đổi mới vào vùng chờ
git commit -m "Mô tả thay đổi"         # Tạo commit mới cho các thay đổi
git push                              # Đẩy nhanh code lên nhánh hiện tại
git push --force                      # nếu chắc chắn muốn xóa các file trên Git thay bằng file mới trên máy tính
```

## 5. Hướng dẫn tải dự án từ GitHub về máy tính mới (Ổ C)
Mở Terminal tại thư mục muốn lưu ở ổ C (ví dụ: `C:\`) rồi chạy lệnh:
```bash
git clone https://github.com/anhtn1980/tnn-smart-control.git
```
Sau đó checkout đúng branch đang làm việc
```bash
cd tnn-smart-control
git checkout claude/gifted-darwin-emresu
```
Nếu đã clone rồi, chỉ cần pull về:
```bash
git pull origin claude/gifted-darwin-emresu
```

## 6. Các lệnh kiểm tra thông dụng khác
```bash
git status                            # Kiểm tra trạng thái các file hiện tại
git log --oneline                     # Xem lịch sử các commit một cách ngắn gọn
```

