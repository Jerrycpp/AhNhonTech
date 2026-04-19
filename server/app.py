import os
import sqlite3
from datetime import datetime, timezone
from uuid import uuid4

from flask import Flask, jsonify, request, send_from_directory, url_for
from werkzeug.utils import secure_filename

app = Flask(__name__)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MEDIA_DIR = os.path.join(BASE_DIR, "media")
VIDEOS_DIR = os.path.join(MEDIA_DIR, "videos")
THUMBS_DIR = os.path.join(MEDIA_DIR, "thumbnails")
DATA_DIR = os.path.join(BASE_DIR, "data")
DB_PATH = os.path.join(DATA_DIR, "gallery.db")
ALLOWED_MJPEG_EXTENSIONS = {".mjpeg", ".mjpg"}
ALLOWED_THUMBNAIL_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp"}

for directory in (VIDEOS_DIR, THUMBS_DIR, DATA_DIR):
    os.makedirs(directory, exist_ok=True)

current_video_id = None


def utc_now_iso():
    return datetime.now(timezone.utc).isoformat()


def is_allowed_mjpeg_filename(filename):
    _, extension = os.path.splitext(filename)
    return extension.lower() in ALLOWED_MJPEG_EXTENSIONS


def is_allowed_thumbnail_filename(filename):
    _, extension = os.path.splitext(filename)
    return extension.lower() in ALLOWED_THUMBNAIL_EXTENSIONS


def get_db_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with get_db_connection() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS videos (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_filename TEXT NOT NULL,
                video_filename TEXT NOT NULL,
                thumbnail_filename TEXT,
                thumb_time_sec REAL,
                thumbnail_status TEXT NOT NULL DEFAULT 'pending',
                deploy_selected INTEGER NOT NULL DEFAULT 0,
                deploy_order INTEGER,
                uploaded_at TEXT NOT NULL,
                played_at TEXT
            )
            """
        )
        columns = {row["name"] for row in conn.execute("PRAGMA table_info(videos)").fetchall()}
        if "deploy_selected" not in columns:
            conn.execute(
                "ALTER TABLE videos ADD COLUMN deploy_selected INTEGER NOT NULL DEFAULT 0"
            )
        if "deploy_order" not in columns:
            conn.execute("ALTER TABLE videos ADD COLUMN deploy_order INTEGER")
        conn.commit()


def fetch_video(video_id):
    with get_db_connection() as conn:
        return conn.execute(
            "SELECT * FROM videos WHERE id = ?",
            (video_id,),
        ).fetchone()


def serialize_video(row):
    if row is None:
        return None

    video_path = os.path.join(VIDEOS_DIR, row["video_filename"])
    thumbnail_path = (
        os.path.join(THUMBS_DIR, row["thumbnail_filename"])
        if row["thumbnail_filename"]
        else None
    )

    return {
        "video_id": row["id"],
        "source_filename": row["source_filename"],
        "video_filename": row["video_filename"],
        "thumbnail_filename": row["thumbnail_filename"],
        "thumb_time_sec": row["thumb_time_sec"],
        "thumbnail_status": row["thumbnail_status"],
        "deploy_selected": bool(row["deploy_selected"]),
        "deploy_order": row["deploy_order"],
        "uploaded_at": row["uploaded_at"],
        "played_at": row["played_at"],
        "video_size_bytes": (
            os.path.getsize(video_path) if os.path.exists(video_path) else None
        ),
        "thumbnail_size_bytes": (
            os.path.getsize(thumbnail_path)
            if thumbnail_path and os.path.exists(thumbnail_path)
            else None
        ),
        "video_url": url_for("stream_video", video_id=row["id"], _external=True),
        "thumbnail_url": (
            url_for("video_thumbnail", video_id=row["id"], _external=True)
            if row["thumbnail_filename"]
            else None
        ),
    }


@app.route("/", methods=["GET"])
def home():
    return jsonify(
        {
            "service": "gallery-server",
            "status": "running",
            "endpoints": {
                "gallery": "/gallery",
                "current": "/current",
                "upload_video": "/videos/upload",
                "play_video": "/play",
                "stream_video": "/videos/<video_id>/stream",
                "video_thumbnail": "/videos/<video_id>/thumbnail",
                "deploy_selection_get": "/deploy/selection",
                "deploy_selection_set": "/deploy/selection",
                "deploy_manifest": "/deploy/manifest",
            },
        }
    )


@app.route("/gallery", methods=["GET"])
def gallery():
    with get_db_connection() as conn:
        rows = conn.execute("SELECT * FROM videos ORDER BY id DESC").fetchall()
    return jsonify({"count": len(rows), "items": [serialize_video(row) for row in rows]})


@app.route("/videos/<int:video_id>", methods=["GET"])
def video_detail(video_id):
    row = fetch_video(video_id)
    if row is None:
        return jsonify({"error": "Video not found", "video_id": video_id}), 404
    return jsonify({"video": serialize_video(row)})


@app.route("/deploy/selection", methods=["GET"])
def get_deploy_selection():
    with get_db_connection() as conn:
        rows = conn.execute(
            """
            SELECT * FROM videos
            WHERE deploy_selected = 1
            ORDER BY deploy_order ASC, id ASC
            """
        ).fetchall()

    return jsonify({"count": len(rows), "items": [serialize_video(row) for row in rows]})


@app.route("/deploy/selection", methods=["POST"])
def set_deploy_selection():
    payload = request.get_json(silent=True) or {}
    requested_ids = payload.get("video_ids")
    if requested_ids is None:
        return jsonify({"error": "JSON body must include 'video_ids' list"}), 400
    if not isinstance(requested_ids, list):
        return jsonify({"error": "'video_ids' must be a list"}), 400

    unique_ids = []
    seen = set()
    for value in requested_ids:
        try:
            video_id = int(value)
        except (TypeError, ValueError):
            return jsonify({"error": f"Invalid video_id '{value}' (must be integer)"}), 400
        if video_id in seen:
            continue
        seen.add(video_id)
        unique_ids.append(video_id)

    with get_db_connection() as conn:
        if unique_ids:
            placeholders = ",".join("?" for _ in unique_ids)
            rows = conn.execute(
                f"SELECT id FROM videos WHERE id IN ({placeholders})",
                tuple(unique_ids),
            ).fetchall()
            existing_ids = {row["id"] for row in rows}
            missing_ids = [video_id for video_id in unique_ids if video_id not in existing_ids]
            if missing_ids:
                return (
                    jsonify({"error": "Some video_ids were not found", "missing_video_ids": missing_ids}),
                    404,
                )

        conn.execute("UPDATE videos SET deploy_selected = 0, deploy_order = NULL")
        for index, video_id in enumerate(unique_ids):
            conn.execute(
                """
                UPDATE videos
                SET deploy_selected = 1, deploy_order = ?
                WHERE id = ?
                """,
                (index, video_id),
            )

        rows = conn.execute(
            """
            SELECT * FROM videos
            WHERE deploy_selected = 1
            ORDER BY deploy_order ASC, id ASC
            """
        ).fetchall()
        conn.commit()

    return jsonify(
        {
            "status": "ok",
            "count": len(rows),
            "items": [serialize_video(row) for row in rows],
        }
    )


@app.route("/deploy/manifest", methods=["GET"])
def deploy_manifest():
    limit_param = request.args.get("limit")
    limit = None
    if limit_param is not None:
        try:
            limit = int(limit_param)
        except ValueError:
            return jsonify({"error": "limit must be an integer"}), 400
        if limit < 1:
            return jsonify({"error": "limit must be >= 1"}), 400

    query = """
        SELECT * FROM videos
        WHERE deploy_selected = 1
        ORDER BY deploy_order ASC, id ASC
    """
    params = ()
    if limit is not None:
        query += " LIMIT ?"
        params = (limit,)

    with get_db_connection() as conn:
        rows = conn.execute(query, params).fetchall()

    items = [serialize_video(row) for row in rows]
    return jsonify({"generated_at": utc_now_iso(), "count": len(items), "items": items})


@app.route("/current", methods=["GET"])
def current():
    global current_video_id
    if current_video_id is None:
        return jsonify({"current": None})

    row = fetch_video(current_video_id)
    if row is None:
        current_video_id = None
        return jsonify({"current": None})

    return jsonify({"current": serialize_video(row)})


@app.route("/videos/upload", methods=["POST"])
@app.route("/upload_video", methods=["POST"])
def upload_video():
    if "video" not in request.files:
        return (
            jsonify(
                {
                    "error": "No video uploaded. Use multipart/form-data with field name 'video'."
                }
            ),
            400,
        )
    if "thumbnail" not in request.files:
        return (
            jsonify(
                {
                    "error": "No thumbnail uploaded. Use multipart/form-data with field name 'thumbnail'."
                }
            ),
            400,
        )

    video_file = request.files["video"]
    thumbnail_file = request.files["thumbnail"]

    if not video_file.filename or not video_file.filename.strip():
        return jsonify({"error": "Uploaded video filename is empty"}), 400
    if not thumbnail_file.filename or not thumbnail_file.filename.strip():
        return jsonify({"error": "Uploaded thumbnail filename is empty"}), 400

    source_filename = secure_filename(video_file.filename)
    if not source_filename:
        return jsonify({"error": "Invalid video filename"}), 400
    if not is_allowed_mjpeg_filename(source_filename):
        return (
            jsonify(
                {
                    "error": "Only MJPEG uploads are supported (.mjpeg or .mjpg)."
                }
            ),
            400,
        )

    source_thumbnail_filename = secure_filename(thumbnail_file.filename)
    if not source_thumbnail_filename:
        return jsonify({"error": "Invalid thumbnail filename"}), 400
    if not is_allowed_thumbnail_filename(source_thumbnail_filename):
        return (
            jsonify(
                {
                    "error": "Thumbnail must be one of: .jpg, .jpeg, .png, .webp."
                }
            ),
            400,
        )

    video_filename = f"{uuid4().hex[:12]}_{source_filename}"
    thumbnail_filename = f"{uuid4().hex[:12]}_{source_thumbnail_filename}"
    video_path = os.path.join(VIDEOS_DIR, video_filename)
    thumbnail_path = os.path.join(THUMBS_DIR, thumbnail_filename)

    video_file.save(video_path)
    thumbnail_file.save(thumbnail_path)

    with get_db_connection() as conn:
        cursor = conn.execute(
            """
            INSERT INTO videos (
                source_filename,
                video_filename,
                thumbnail_filename,
                thumb_time_sec,
                thumbnail_status,
                uploaded_at
            ) VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                source_filename,
                video_filename,
                thumbnail_filename,
                None,
                "uploaded",
                utc_now_iso(),
            ),
        )
        video_id = cursor.lastrowid

        row = conn.execute("SELECT * FROM videos WHERE id = ?", (video_id,)).fetchone()
        conn.commit()

    return (
        jsonify(
            {
                "status": "ok",
                "video": serialize_video(row),
            }
        ),
        201,
    )


@app.route("/play", methods=["POST"])
def play():
    global current_video_id

    payload = request.get_json(silent=True) or {}
    video_id = payload.get("video_id")
    if video_id is None:
        form_video_id = request.form.get("video_id")
        video_id = form_video_id if form_video_id not in (None, "") else None

    with get_db_connection() as conn:
        if video_id is None:
            row = conn.execute("SELECT * FROM videos ORDER BY id DESC LIMIT 1").fetchone()
            if row is None:
                return jsonify({"error": "No videos in gallery"}), 404
        else:
            try:
                selected_id = int(video_id)
            except (TypeError, ValueError):
                return jsonify({"error": "video_id must be an integer"}), 400

            row = conn.execute("SELECT * FROM videos WHERE id = ?", (selected_id,)).fetchone()
            if row is None:
                return jsonify({"error": "Video not found", "video_id": selected_id}), 404

        current_video_id = row["id"]
        conn.execute(
            "UPDATE videos SET played_at = ? WHERE id = ?",
            (utc_now_iso(), current_video_id),
        )
        row = conn.execute(
            "SELECT * FROM videos WHERE id = ?",
            (current_video_id,),
        ).fetchone()
        conn.commit()

    return jsonify({"status": "play requested", "current": serialize_video(row)})


@app.route("/videos/<int:video_id>/stream", methods=["GET"])
def stream_video(video_id):
    row = fetch_video(video_id)
    if row is None:
        return jsonify({"error": "Video not found", "video_id": video_id}), 404

    return send_from_directory(VIDEOS_DIR, row["video_filename"], as_attachment=False)


@app.route("/videos/<int:video_id>/thumbnail", methods=["GET"])
def video_thumbnail(video_id):
    row = fetch_video(video_id)
    if row is None:
        return jsonify({"error": "Video not found", "video_id": video_id}), 404

    if not row["thumbnail_filename"]:
        return jsonify({"error": "Thumbnail not set for this video", "video_id": video_id}), 404

    return send_from_directory(THUMBS_DIR, row["thumbnail_filename"], as_attachment=False)


@app.route("/thumbnail/<path:filename>", methods=["GET"])
def thumbnail_by_filename(filename):
    return send_from_directory(THUMBS_DIR, filename, as_attachment=False)


@app.route("/video/<path:filename>", methods=["GET"])
def video_by_filename(filename):
    return send_from_directory(VIDEOS_DIR, filename, as_attachment=False)


init_db()


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
