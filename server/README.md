# Starkhack Gallery Server

Simple Flask server for storing and serving MJPEG videos with uploaded thumbnails.

No AI runs on the server. The Pi uploads a video + thumbnail pair, and clients (including ESP32) pull the selected media from API endpoints.

## Requirements

- Python 3.9+ (recommended)
- `pip`

## Setup

### Windows PowerShell

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install flask
```

### macOS/Linux

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
pip install flask
```

## Run Server

```bash
python app.py
```

Server listens on:

- `http://0.0.0.0:5000` (all interfaces)
- `http://<SERVER_IP>:5000` from other devices on LAN

## Pi Upload Command (Video + Thumbnail)

Upload uses multipart form fields:

- `video` (`.mjpeg` or `.mjpg`)
- `thumbnail` (`.jpg`, `.jpeg`, `.png`, `.webp`)

```bash
curl -X POST "http://<SERVER_IP>:5000/videos/upload" \
  -F "video=@clip.mjpeg" \
  -F "thumbnail=@clip_thumb.jpg"
```

Legacy upload route also works:

- `POST /upload_video`

## ESP32 / Client Pull Flow

### 1) See gallery

```bash
curl "http://<SERVER_IP>:5000/gallery"
```

### 2) Set deploy selection (ordered list)

```bash
curl -X POST "http://<SERVER_IP>:5000/deploy/selection" \
  -H "Content-Type: application/json" \
  -d "{\"video_ids\":[3,1,2]}"
```

### 3) Pull deploy manifest

```bash
curl "http://<SERVER_IP>:5000/deploy/manifest"
```

Optional limit:

```bash
curl "http://<SERVER_IP>:5000/deploy/manifest?limit=1"
```

### 4) Start playback state (optional server-side current pointer)

```bash
curl -X POST "http://<SERVER_IP>:5000/play" \
  -H "Content-Type: application/json" \
  -d "{\"video_id\":3}"
```

### 5) Read current playback item

```bash
curl "http://<SERVER_IP>:5000/current"
```

### 6) Pull media files

```bash
curl "http://<SERVER_IP>:5000/videos/3/stream" --output video.mjpeg
curl "http://<SERVER_IP>:5000/videos/3/thumbnail" --output thumb.jpg
```

## API Quick Reference

- `GET /` service info
- `GET /gallery` all uploaded videos
- `GET /videos/<video_id>` single video metadata
- `POST /videos/upload` upload video + thumbnail
- `POST /upload_video` legacy alias of upload
- `POST /play` set current video
- `GET /current` get current video
- `GET /videos/<video_id>/stream` fetch video file
- `GET /videos/<video_id>/thumbnail` fetch thumbnail file
- `GET /deploy/selection` get selected deploy list
- `POST /deploy/selection` set selected deploy list (ordered)
- `GET /deploy/manifest` get deploy payload for pull clients

## Data Storage

Created automatically on first run:

- `media/videos/`
- `media/thumbnails/`
- `data/gallery.db`
