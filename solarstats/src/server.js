import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import "dotenv/config";
import express from "express";
import { WebSocketServer } from "ws";
import {
  getHistory,
  insertSample,
  openDatabase,
  pruneOldSamples,
} from "./db.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8787);
const INGEST_SECRET = process.env.INGEST_SECRET || "";
const DB_PATH = process.env.DB_PATH || "./data/solarstats.db";
const HISTORY_RETENTION_DAYS = Number(process.env.HISTORY_RETENTION_DAYS || 30);

const db = openDatabase(DB_PATH);
const app = express();
app.use(express.json({ limit: "64kb" }));

const publicDir = path.join(__dirname, "..", "public");
app.use(express.static(publicDir));

function authorizeIngest(req, res, next) {
  if (!INGEST_SECRET) {
    return next();
  }
  const header = req.get("authorization") || "";
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  if (token !== INGEST_SECRET) {
    return res.status(401).json({ error: "unauthorized" });
  }
  return next();
}

app.get("/", (_req, res) => {
  res.sendFile(path.join(publicDir, "solarstats.html"));
});

// Back-compat for old bookmarks
app.get("/solarstats", (_req, res) => {
  res.redirect(301, "/");
});

app.get("/api/history", (req, res) => {
  const range = String(req.query.range || "24h");
  res.json(getHistory(db, range));
});

app.post("/api/ingest", authorizeIngest, (req, res) => {
  try {
    const sample = insertSample(db, req.body || {});
    broadcast({ type: "sample", sample });
    res.json({ ok: true, sample });
  } catch (err) {
    console.error("ingest failed:", err);
    res.status(400).json({ error: err.message || "bad request" });
  }
});

app.get("/api/health", (_req, res) => {
  res.json({ ok: true });
});

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: "/ws" });

function broadcast(message) {
  const data = JSON.stringify(message);
  for (const client of wss.clients) {
    if (client.readyState === 1) {
      client.send(data);
    }
  }
}

wss.on("connection", (socket) => {
  const history = getHistory(db, "24h");
  socket.send(JSON.stringify({ type: "history", ...history }));
});

setInterval(() => {
  try {
    pruneOldSamples(db, HISTORY_RETENTION_DAYS);
  } catch (err) {
    console.error("prune failed:", err.message);
  }
}, 6 * 3600000);

server.listen(PORT, () => {
  console.log(`solarstats listening on http://0.0.0.0:${PORT}`);
  console.log(`dashboard: http://127.0.0.1:${PORT}/`);
});
