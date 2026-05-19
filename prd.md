# PRD: Photobox Self-Service System

## 1. Product overview

### 1.1 Document title and version

- PRD: Photobox Self-Service System
- Version: 1.0

### 1.2 Product summary

Photobox Self-Service System adalah sistem photo booth mandiri berbasis kiosk yang memungkinkan pengunjung untuk membayar, mengambil foto, menerapkan overlay/template, lalu menerima hasilnya dalam bentuk cetak langsung maupun unduhan digital melalui QR code — semuanya tanpa memerlukan bantuan operator.

Sistem ini terdiri dari beberapa komponen yang terintegrasi: **Camera Service** (C++ REST API yang sudah sebagian dibangun), **Kiosk UI** (antarmuka sentuh untuk pengunjung), **Admin Panel** (untuk manajemen overlay, konfigurasi sistem, dan monitoring), serta integrasi **Xendit** sebagai payment gateway dan **printer lokal** untuk cetak foto langsung.

Kamera yang digunakan adalah Sony A7III (ILCE-7M3) yang dikontrol melalui protokol PTP/WIA via USB, dengan Camera Service berjalan sebagai microservice lokal di Windows. Seluruh alur — dari pembayaran hingga pengiriman foto — dirancang untuk berjalan secara otomatis dan ramah pengguna.

## 2. Goals

### 2.1 Business goals

- Menghadirkan pengalaman photo booth berkualitas kamera mirrorless (Sony A7III) dengan operasional minimal tanpa operator
- Menghasilkan revenue per sesi dari pembayaran digital via Xendit
- Mendukung kustomisasi visual (overlay/template) untuk keperluan branding event atau sponsor
- Meminimalkan downtime dengan error recovery otomatis pada kamera dan printer

### 2.2 User goals

- Pengunjung dapat menyelesaikan seluruh alur (bayar → foto → cetak/unduh) secara mandiri dalam waktu singkat
- Pengunjung mendapatkan foto berkualitas tinggi dengan overlay/frame yang menarik
- Pengunjung dapat mengunduh foto digital kapan saja melalui QR code tanpa perlu aplikasi tambahan
- Operator/admin dapat mengubah overlay dan konfigurasi sistem tanpa perlu mengubah kode

### 2.3 Non-goals

- Tidak mendukung kamera selain Sony A7III (ILCE-7M3) pada versi pertama
- Tidak mendukung pembayaran tunai atau payment gateway selain Xendit pada versi pertama
- Tidak menyediakan fitur editing foto tingkat lanjut (retouching, filter AI, dll.)
- Tidak mendukung multi-kamera dalam satu sesi
- Tidak tersedia sebagai layanan cloud — sistem berjalan sepenuhnya lokal (on-premise Windows)

## 3. User personas

### 3.1 Key user types

- Pengunjung (Customer) — pengguna akhir yang menggunakan kiosk secara mandiri
- Admin/Operator — pengelola yang mengonfigurasi sistem, overlay, dan memantau sesi

### 3.2 Basic persona details

- **Pengunjung**: Individu atau kelompok yang datang ke lokasi photobox. Tidak memiliki pengetahuan teknis. Mengharapkan pengalaman yang cepat, intuitif, dan menyenangkan. Membayar via QRIS/transfer/kartu melalui Xendit sebelum sesi dimulai.
- **Admin/Operator**: Pengelola venue atau event organizer yang bertanggung jawab atas konten visual (overlay), konfigurasi harga paket, dan monitoring sistem. Mengakses sistem melalui Admin Panel.

### 3.3 Role-based access

- **Pengunjung**: Akses hanya ke Kiosk UI — dapat memilih paket, membayar, menjalankan sesi foto, memilih overlay, mencetak, dan mengunduh via QR code
- **Admin**: Akses ke Admin Panel — dapat mengelola overlay/template, mengatur paket dan harga, melihat riwayat sesi dan transaksi, mengonfigurasi kamera dan printer, serta me-restart sesi darurat

## 4. Functional requirements

- **Manajemen koneksi kamera** (Priority: High)
  - Camera Service otomatis melakukan SDIO handshake saat startup
  - Auto-reconnect jika koneksi kamera terputus saat sesi tidak aktif
  - Endpoint `POST /disconnect` untuk melepas koneksi kamera secara bersih
  - Status koneksi real-time tersedia via `GET /camera-status`

- **Alur pembayaran via Xendit** (Priority: High)
  - Pengunjung memilih paket sesi (jumlah foto, durasi)
  - Sistem membuat payment request ke Xendit API
  - Kiosk UI menampilkan QR code / instruksi pembayaran
  - Sistem polling status pembayaran; sesi terbuka otomatis setelah pembayaran terkonfirmasi
  - Pembayaran yang kedaluwarsa (timeout) direset dan pengunjung diarahkan kembali ke halaman awal

- **Sesi foto dengan live view** (Priority: High)
  - Kiosk UI menampilkan live view dari kamera secara real-time via MJPEG stream (`GET /liveview`)
  - Countdown timer tampil di layar sebelum shutter dilepas
  - Sistem mengambil foto sesuai jumlah yang ditentukan paket
  - Jeda antar foto (interval) dapat dikonfigurasi admin
  - Foto tersimpan otomatis ke direktori lokal dengan nama berbasis timestamp

- **Pemilihan dan penerapan overlay** (Priority: High)
  - Setelah sesi selesai, pengunjung memilih satu overlay/template dari daftar yang tersedia
  - Overlay diterapkan ke seluruh foto dalam sesi secara otomatis (compositing)
  - Sistem menghasilkan versi final foto dengan overlay tertanam

- **Cetak foto langsung** (Priority: High)
  - Pengunjung dapat memilih opsi cetak foto setelah sesi
  - Sistem mengirim perintah cetak ke printer lokal yang terhubung
  - Status cetak (antrian, sedang cetak, selesai, error) ditampilkan di layar

- **Unduhan digital via QR code** (Priority: High)
  - Sistem menghasilkan QR code unik per sesi yang mengarah ke URL unduhan foto
  - Pengunjung dapat memindai QR code untuk mengunduh foto final (dengan overlay) dalam format JPEG
  - Link unduhan memiliki masa berlaku yang dapat dikonfigurasi (default: 24 jam)

- **Manajemen overlay di Admin Panel** (Priority: High)
  - Admin dapat mengunggah, mengaktifkan, menonaktifkan, dan menghapus overlay/template
  - Overlay mendukung format PNG dengan transparansi (alpha channel)
  - Admin dapat mengatur urutan tampilan overlay di kiosk

- **Manajemen paket di Admin Panel** (Priority: Medium)
  - Admin dapat membuat, mengedit, dan menonaktifkan paket (nama, harga, jumlah foto, durasi sesi)
  - Harga didenominasikan dalam IDR

- **Monitoring dan riwayat sesi** (Priority: Medium)
  - Admin dapat melihat daftar sesi (tanggal, paket, status pembayaran, jumlah foto)
  - Admin dapat mengunduh atau melihat foto dari sesi tertentu
  - Dashboard menampilkan ringkasan revenue dan jumlah sesi harian/mingguan

- **Konfigurasi kamera via Admin Panel** (Priority: Medium)
  - Admin dapat mengatur ISO, aperture, shutter speed, white balance, dan drive mode via UI
  - Perubahan dikirim ke Camera Service via `POST /camera/settings` (endpoint baru)

- **Camera Settings API** (Priority: Medium)
  - Endpoint `GET /camera/settings` untuk membaca nilai properti kamera saat ini
  - Endpoint `POST /camera/settings` untuk mengatur ISO, aperture, shutter speed, white balance via `SDIOSetExtDevicePropValue`

- **Penggantian library JSON** (Priority: Low)
  - Mengganti manual JSON string building di `main.cpp` dengan nlohmann/json untuk keandalan dan keamanan

## 5. User experience

### 5.1 Entry points & first-time user flow

- Pengunjung mendekati kiosk dan layar menampilkan halaman selamat datang dengan instruksi singkat
- Pengunjung mengetuk layar untuk memulai dan memilih paket yang diinginkan
- Sistem mengarahkan ke halaman pembayaran Xendit

### 5.2 Core experience

- **Pilih paket**: Pengunjung memilih paket (misal: 4 foto, 3 menit sesi) dan melihat harga sebelum melanjutkan
- **Bayar**: Xendit payment QR / instruksi ditampilkan; pengunjung menyelesaikan pembayaran dari smartphone; layar update otomatis saat pembayaran terkonfirmasi
- **Persiapan sesi**: Layar menampilkan hitungan mundur singkat (3-2-1) dan instruksi posisi
- **Ambil foto**: Live view ditampilkan full-screen; countdown muncul sebelum setiap jepretan; total jepretan sesuai paket
- **Pilih overlay**: Galeri overlay ditampilkan; pengunjung memilih satu; preview langsung ditampilkan di atas foto
- **Terima hasil**: Pengunjung memilih cetak, unduh via QR code, atau keduanya; QR code ditampilkan di layar hingga dipindai

### 5.3 Advanced features & edge cases

- Jika kamera terputus di tengah sesi, sistem menampilkan pesan error dan membekukan sesi (tidak menghapus data pembayaran)
- Jika printer error, pengunjung tetap dapat mengunduh foto via QR code tanpa kehilangan sesi
- Jika pembayaran timeout, sesi direset dan inventori tidak terpakai
- Admin dapat membuka sesi darurat (complimentary) tanpa pembayaran dari Admin Panel
- Sesi yang tidak selesai (pengunjung pergi) akan expired setelah timeout yang dikonfigurasi

### 5.4 UI/UX highlights

- Antarmuka sentuh penuh dengan elemen besar, kontras tinggi, dan teks dalam Bahasa Indonesia
- Animasi transisi antar layar yang smooth untuk kesan premium
- Countdown visual yang mencolok (animasi besar, suara opsional) sebelum setiap jepretan
- Live view full-screen untuk pengalaman foto yang imersif
- QR code besar dan mudah dipindai di layar akhir

## 6. Narrative

Seorang pengunjung mendekati kiosk photobox di sebuah event, memilih paket 4 foto, dan menyelesaikan pembayaran via QRIS dalam hitungan detik. Kiosk langsung membuka sesi foto: live view Sony A7III tampil di layar besar, countdown berjalan, dan empat momen terabadikan dengan kualitas mirrorless. Pengunjung memilih overlay bertema event, lalu dalam satu ketukan memilih cetak sekaligus unduh — printer langsung mencetak hasilnya sementara QR code di layar memungkinkan mereka berbagi foto digital ke media sosial saat itu juga. Seluruh pengalaman, dari tap pertama hingga foto di tangan, selesai dalam kurang dari 5 menit tanpa satu pun operator yang terlibat.

## 7. Success metrics

### 7.1 User-centric metrics

- Waktu rata-rata penyelesaian sesi (dari tap pertama hingga foto diterima) ≤ 5 menit
- Tingkat keberhasilan sesi (sesi yang selesai tanpa error) ≥ 95%
- Tingkat kepuasan pengunjung ≥ 4.0/5.0 (jika survei diimplementasikan)

### 7.2 Business metrics

- Revenue per hari (jumlah sesi × rata-rata nilai transaksi)
- Tingkat konversi pengunjung (yang memulai pembayaran) ≥ 80%
- Uptime sistem ≥ 99% selama jam operasional

### 7.3 Technical metrics

- Latensi live view frame ≤ 100ms (target ~30fps)
- Waktu capture-to-display (foto tersedia di layar setelah dijepret) ≤ 3 detik
- Waktu compositing overlay ≤ 2 detik per foto
- Waktu generate QR code + link unduhan ≤ 1 detik setelah foto diproses
- Tidak ada memory leak pada Camera Service setelah 8 jam operasional berkesinambungan

## 8. Technical considerations

### 8.1 Integration points

- **Camera Service** (C++ / `CameraService.exe`): REST API lokal di `localhost:8080` — sudah ada, perlu ditambah endpoint settings dan disconnect
- **Xendit API**: Payment request, status polling (webhook atau polling), dan pembatalan transaksi
- **Printer lokal**: Integrasi via Windows Print Spooler API atau WIA (tergantung printer yang digunakan)
- **Kiosk UI**: Frontend (React/Electron direkomendasikan untuk kiosk Windows) berkomunikasi dengan Camera Service via HTTP
- **Admin Panel**: Aplikasi web terpisah (atau route terproteksi di dalam kiosk UI) untuk manajemen overlay dan konfigurasi
- **File storage lokal**: Foto raw dan foto dengan overlay disimpan di direktori lokal (`C:\photobox\captures` dan `C:\photobox\processed`)
- **Link unduhan**: File server lokal (atau upload ke cloud storage seperti S3/Cloudflare R2) untuk generate URL yang bisa diakses via QR code

### 8.2 Data storage & privacy

- Foto pengunjung disimpan lokal dan dihapus otomatis setelah masa berlaku link unduhan berakhir (default: 24 jam)
- Tidak ada data pribadi (nama, nomor telepon) yang dikumpulkan dari pengunjung kecuali data transaksi Xendit
- Data transaksi Xendit (payment ID, jumlah, timestamp) disimpan lokal untuk rekonsiliasi
- Admin credentials untuk Admin Panel disimpan dengan hashing yang kuat (bcrypt)
- Koneksi ke Xendit API menggunakan HTTPS dan API key disimpan di environment variable, bukan hardcoded

### 8.3 Scalability & performance

- Sistem dirancang untuk single-kiosk, single-camera; scaling horizontal bukan prioritas v1
- Camera Service menggunakan mutex untuk thread safety pada semua operasi PTP (sudah ada)
- Background polling thread (`POLL_INTERVAL_MS = 1000ms`) harus dioptimasi untuk tidak mengganggu operasi capture
- Compositing overlay sebaiknya menggunakan library C++ yang efisien (misal: stb_image atau WIC) atau dilakukan di sisi frontend
- File unduhan harus di-serve dengan header `Cache-Control` dan kompresi yang tepat

### 8.4 Potential challenges

- **Sony A7III tidak didukung CRSDK resmi** — menggunakan PTP/WIA alternatif yang lebih rapuh; error handling harus robust
- **WIA single-client constraint** — hanya satu aplikasi yang bisa terhubung ke kamera; restart Camera Service memerlukan reconnect penuh
- **Latensi live view** — GetObject via PTP memiliki overhead; perlu diukur dan dioptimasi untuk pengalaman live view yang smooth
- **Printer compatibility** — variasi driver printer Windows memerlukan abstraksi yang fleksibel
- **Xendit webhook vs. polling** — di lingkungan kiosk tanpa public IP, polling status pembayaran lebih mudah diimplementasikan tapi lebih lambat dari webhook
- **Overlay compositing** — penerapan PNG overlay ke JPEG dengan alpha blending memerlukan library image processing yang tepat

## 9. Milestones & sequencing

### 9.1 Project estimate

- **Large**: 10–14 minggu (tim kecil 2–3 developer)

### 9.2 Team size & composition

- **2–3 developer**: 1 backend C++ developer, 1 frontend developer (Kiosk UI + Admin Panel), 1 fullstack/integrator (opsional untuk payment & file server)

### 9.3 Suggested phases

- **Phase 1 — Camera Service Completion** (2–3 minggu)
  - Implementasi `POST /disconnect` endpoint
  - Implementasi `GET /camera/settings` dan `POST /camera/settings` (ISO, aperture, shutter, WB, drive mode)
  - Auto-reconnect logic saat kamera terputus
  - Ganti manual JSON string building dengan nlohmann/json
  - Stabilisasi MJPEG stream (`GET /liveview`) dan single-frame endpoint (`GET /liveview/frame`)

- **Phase 2 — Kiosk UI Core** (3–4 minggu)
  - Setup project Electron/React untuk kiosk Windows (fullscreen, touch-friendly)
  - Implementasi alur: Welcome → Pilih Paket → Live View → Countdown → Capture → Pilih Overlay → Cetak/Unduh
  - Integrasi dengan Camera Service API (connect, live view, capture, images)
  - Overlay compositing di sisi frontend (canvas API atau sharp library)

- **Phase 3 — Payment & Delivery** (2–3 minggu)
  - Integrasi Xendit API: buat transaksi, polling status, handle timeout & cancellation
  - Implementasi QR code generation untuk link unduhan
  - File server lokal (atau integrasi cloud storage) untuk URL unduhan
  - Integrasi printer lokal (Windows Print Spooler)
  - Session lifecycle management (paid → active → completed → expired)

- **Phase 4 — Admin Panel** (2 minggu)
  - CRUD overlay/template (upload PNG, aktif/nonaktif, urutan)
  - Manajemen paket (nama, harga, jumlah foto, durasi)
  - Dashboard riwayat sesi dan transaksi
  - Konfigurasi kamera via UI (kirim ke Camera Service `/camera/settings`)
  - Autentikasi admin (login dengan password)

- **Phase 5 — QA, Hardening & Deployment** (1–2 minggu)
  - End-to-end testing seluruh alur di hardware nyata
  - Stress test: 8 jam operasional berkesinambungan
  - Error recovery testing (cabut kabel kamera, matikan printer, timeout pembayaran)
  - Setup auto-start Camera Service dan Kiosk UI saat Windows boot
  - Dokumentasi operasional untuk admin

## 10. User stories

### 10.1. Pengunjung memilih paket sesi

- **ID**: PB-001
- **Description**: Sebagai pengunjung, saya ingin melihat daftar paket yang tersedia beserta harga dan jumlah foto, sehingga saya dapat memilih paket yang sesuai dengan kebutuhan saya.
- **Acceptance criteria**:
  - Halaman paket menampilkan minimal satu paket dengan nama, harga (IDR), jumlah foto, dan estimasi durasi
  - Pengunjung dapat mengetuk satu paket untuk memilihnya
  - Tombol "Lanjut ke Pembayaran" aktif hanya setelah satu paket dipilih
  - Paket yang dinonaktifkan admin tidak tampil di kiosk

### 10.2. Pengunjung melakukan pembayaran via Xendit

- **ID**: PB-002
- **Description**: Sebagai pengunjung, saya ingin membayar sesi foto menggunakan metode pembayaran digital (QRIS/transfer), sehingga saya bisa memulai sesi tanpa uang tunai.
- **Acceptance criteria**:
  - Sistem membuat payment request ke Xendit dan menampilkan QR code / instruksi pembayaran dalam ≤ 3 detik
  - QR code / instruksi tampil jelas di layar
  - Countdown timer pembayaran (misal: 5 menit) ditampilkan
  - Layar berpindah otomatis ke persiapan sesi dalam ≤ 5 detik setelah pembayaran terkonfirmasi
  - Jika pembayaran timeout, layar kembali ke halaman awal dengan pesan yang jelas
  - Payment ID Xendit tersimpan di database lokal

### 10.3. Pengunjung melihat live view kamera sebelum foto diambil

- **ID**: PB-003
- **Description**: Sebagai pengunjung, saya ingin melihat preview kamera secara real-time sebelum foto diambil, sehingga saya bisa memposisikan diri dengan tepat.
- **Acceptance criteria**:
  - Live view ditampilkan full-screen segera setelah sesi dimulai
  - Frame live view diperbarui dengan latensi yang terasa real-time (target ≥ 15fps)
  - Jumlah foto yang tersisa dalam sesi ditampilkan di layar
  - Countdown visual (misal: 3-2-1) tampil di atas live view sebelum setiap jepretan

### 10.4. Sistem mengambil foto sesuai paket

- **ID**: PB-004
- **Description**: Sebagai pengunjung, saya ingin kamera mengambil foto secara otomatis sesuai jumlah yang ada di paket, sehingga saya tidak perlu mengoperasikan kamera sendiri.
- **Acceptance criteria**:
  - Jumlah foto yang diambil tepat sesuai paket yang dipilih
  - Jeda antar foto (interval) sesuai konfigurasi admin (default: 3 detik setelah countdown)
  - Foto tersimpan ke direktori lokal dengan nama berbasis timestamp
  - Setelah semua foto diambil, layar berpindah ke halaman pemilihan overlay
  - Jika salah satu capture gagal, sistem mencoba ulang sekali sebelum menampilkan error

### 10.5. Pengunjung memilih overlay/template untuk foto

- **ID**: PB-005
- **Description**: Sebagai pengunjung, saya ingin memilih overlay/frame yang akan diterapkan ke semua foto saya, sehingga hasilnya memiliki tampilan yang menarik dan sesuai tema event.
- **Acceptance criteria**:
  - Halaman overlay menampilkan semua overlay aktif yang dikonfigurasi admin dalam format grid/carousel
  - Ketika pengunjung memilih overlay, preview langsung ditampilkan menggunakan salah satu foto dari sesi
  - Pengunjung dapat mengganti pilihan overlay sebelum mengonfirmasi
  - Tombol "Konfirmasi" menerapkan overlay ke semua foto dalam sesi
  - Proses compositing selesai dalam ≤ 2 detik per foto

### 10.6. Pengunjung mencetak foto

- **ID**: PB-006
- **Description**: Sebagai pengunjung, saya ingin mencetak foto saya langsung dari kiosk, sehingga saya bisa langsung membawa pulang hasil cetaknya.
- **Acceptance criteria**:
  - Opsi cetak tersedia di halaman akhir sesi
  - Klik "Cetak" mengirim perintah cetak ke printer lokal
  - Status cetak (memproses, sedang cetak, selesai) ditampilkan di layar
  - Jika printer error, pesan error yang jelas ditampilkan dan opsi unduhan QR code tetap tersedia
  - Cetak menggunakan foto final (sudah dengan overlay)

### 10.7. Pengunjung mengunduh foto via QR code

- **ID**: PB-007
- **Description**: Sebagai pengunjung, saya ingin mendapatkan QR code untuk mengunduh foto digital saya, sehingga saya bisa menyimpan dan berbagi foto dari smartphone saya.
- **Acceptance criteria**:
  - QR code unik per sesi di-generate dan ditampilkan di layar akhir
  - Memindai QR code membuka URL yang langsung mengunduh foto final (JPEG) atau halaman galeri sesi
  - Link QR code aktif selama periode yang dikonfigurasi admin (default: 24 jam)
  - QR code ditampilkan cukup besar untuk dipindai dari jarak normal berdiri di depan kiosk
  - Setelah periode expired, link menampilkan pesan "link telah kedaluwarsa"

### 10.8. Admin mengelola overlay/template

- **ID**: PB-008
- **Description**: Sebagai admin, saya ingin mengunggah dan mengaktifkan overlay/template dari Admin Panel, sehingga konten visual kiosk dapat disesuaikan untuk setiap event tanpa perlu mengubah kode.
- **Acceptance criteria**:
  - Admin dapat mengunggah file PNG (dengan alpha channel) sebagai overlay baru
  - Admin dapat memberi nama, mengaktifkan, menonaktifkan, dan menghapus overlay
  - Admin dapat mengatur urutan tampilan overlay di kiosk
  - Overlay yang dinonaktifkan tidak muncul di kiosk pengunjung
  - Perubahan overlay langsung berlaku untuk sesi berikutnya tanpa restart sistem

### 10.9. Admin mengelola paket sesi

- **ID**: PB-009
- **Description**: Sebagai admin, saya ingin membuat dan mengedit paket sesi dari Admin Panel, sehingga saya dapat menyesuaikan penawaran dan harga sesuai kebutuhan.
- **Acceptance criteria**:
  - Admin dapat membuat paket baru dengan: nama, harga (IDR), jumlah foto, durasi sesi (menit), dan status aktif/nonaktif
  - Admin dapat mengedit atau menonaktifkan paket yang sudah ada
  - Paket nonaktif tidak ditampilkan di kiosk
  - Perubahan paket berlaku untuk transaksi baru (tidak mengubah transaksi yang sudah berjalan)

### 10.10. Admin memantau riwayat sesi dan transaksi

- **ID**: PB-010
- **Description**: Sebagai admin, saya ingin melihat riwayat semua sesi dan transaksi di Admin Panel, sehingga saya dapat memonitor performa dan melakukan rekonsiliasi.
- **Acceptance criteria**:
  - Dashboard menampilkan total sesi dan total revenue hari ini, minggu ini, dan bulan ini
  - Tabel riwayat sesi menampilkan: tanggal/waktu, paket, payment ID Xendit, status (selesai/gagal/expired)
  - Admin dapat memfilter riwayat berdasarkan rentang tanggal dan status
  - Admin dapat melihat foto-foto dari sesi tertentu

### 10.11. Admin mengonfigurasi pengaturan kamera

- **ID**: PB-011
- **Description**: Sebagai admin, saya ingin mengatur parameter kamera (ISO, aperture, shutter speed, white balance) dari Admin Panel, sehingga kualitas foto dapat disesuaikan tanpa mengakses kamera secara langsung.
- **Acceptance criteria**:
  - Admin Panel menampilkan nilai kamera saat ini (dibaca dari `GET /camera/settings`)
  - Admin dapat mengubah ISO, aperture, shutter speed, white balance, dan drive mode via dropdown/input
  - Perubahan dikirim ke Camera Service dan terkonfirmasi berhasil sebelum disimpan
  - Nilai yang tidak valid (di luar range kamera) ditolak dengan pesan error yang jelas

### 10.12. Sistem melakukan auto-reconnect kamera

- **ID**: PB-012
- **Description**: Sebagai admin, saya ingin sistem secara otomatis mencoba menyambungkan kembali kamera jika koneksi terputus saat tidak ada sesi aktif, sehingga downtime akibat koneksi kamera minimal.
- **Acceptance criteria**:
  - Jika `GET /camera-status` mendeteksi kamera terputus saat idle, sistem mencoba reconnect setelah interval yang dikonfigurasi (default: 30 detik)
  - Maksimal 3 kali percobaan reconnect berturut-turut sebelum menampilkan alert di Admin Panel
  - Sesi yang sedang aktif tidak terinterupsi oleh proses reconnect
  - Status reconnect (attempting, success, failed) tersedia via endpoint dan Admin Panel

### 10.13. Admin login ke Admin Panel

- **ID**: PB-013
- **Description**: Sebagai admin, saya ingin mengakses Admin Panel dengan kredensial yang aman, sehingga konfigurasi sistem tidak dapat diubah oleh sembarang orang.
- **Acceptance criteria**:
  - Admin Panel hanya dapat diakses setelah login dengan username dan password
  - Password disimpan dengan bcrypt hashing
  - Sesi admin memiliki timeout (default: 2 jam tidak aktif)
  - Percobaan login yang gagal dicatat (log) dan dibatasi (rate limiting) untuk mencegah brute force
  - Tidak ada kredensial default yang ter-hardcode; setup awal mewajibkan penggantian password

### 10.14. Pengunjung mendapatkan penanganan error yang jelas

- **ID**: PB-014
- **Description**: Sebagai pengunjung, saya ingin mendapatkan pesan error yang jelas dan actionable jika terjadi masalah teknis, sehingga saya tidak bingung dan tahu apa yang harus dilakukan.
- **Acceptance criteria**:
  - Jika kamera error saat sesi, layar menampilkan pesan dalam Bahasa Indonesia dengan instruksi (misal: "Hubungi petugas")
  - Jika sesi gagal setelah pembayaran berhasil, sistem mencatat insiden dan admin dapat memberikan sesi pengganti dari Admin Panel
  - Error tidak pernah menampilkan pesan teknis/stack trace ke pengunjung
  - Semua error dicatat di log sistem dengan timestamp dan detail teknis untuk debugging admin
