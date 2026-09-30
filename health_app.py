import os
from flask import Flask, jsonify

app = Flask(__name__)

@app.get("/")
def home():
    return "Bot service is alive!"

@app.get("/health")
def health():
    return jsonify({"status": "ok", "service": "her"})

if __name__ == "__main__":
    port = int(os.environ.get("PORT", "10000"))
    app.run(host="0.0.0.0", port=port)
