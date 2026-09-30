# Render deployment

This clean Render service exposes only a Flask health endpoint.

1. Put `render.yaml`, `health_app.py`, and `requirements.txt` in the repository root.
2. In Render, create a Blueprint from the repository.
3. Render will run:
   pip install -r requirements.txt
   gunicorn health_app:app
4. The service listens through Render's PORT environment variable.

Do not store Telegram/API tokens in Git. Put secrets in Render Environment Variables.
