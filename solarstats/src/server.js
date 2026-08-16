import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import "dotenv/config";
import cookieSession from "cookie-session";
import express from "express";
import helmet from "helmet";
import { WebSocketServer } from "ws";
import {
  authConfigured,
  authProviders,
  buildAuthUrl,
  handleAuthCallback,
  webauthnConfig,
} from "./auth.js";
import {
  getAdminEmail,
  isAdminEmail,
  getAuthSettings,
  getHistory,
  getUserById,
  getUserByEmail,
  insertSample,
  listPasskeysForUser,
  listUsers,
  openDatabase,
  pruneOldSamples,
  setAuthSettings,
  setUserStatus,
  upsertMicrosoftUser,
  deletePasskey,
} from "./db.js";
import {
  authenticationOptions,
  registrationOptions,
  verifyAuthentication,
  verifyRegistration,
} from "./passkeys.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8787);
const INGEST_SECRET = process.env.INGEST_SECRET || "";
const DB_PATH = process.env.DB_PATH || "./data/solarstats.db";
const HISTORY_RETENTION_DAYS = Number(process.env.HISTORY_RETENTION_DAYS || 30);
const SESSION_SECRET = process.env.SESSION_SECRET || "dev-insecure-session-secret";

/** ~4 months — stay signed in + has-passkey UX marker. */
const LONG_COOKIE_MS = 120 * 24 * 60 * 60 * 1000;
const HAS_PASSKEY_COOKIE = "solarstats_pk";

const db = openDatabase(DB_PATH);
const app = express();
const publicDir = path.join(__dirname, "..", "public");

app.set("trust proxy", 1);
app.use(
  helmet({
    contentSecurityPolicy: {
      useDefaults: true,
      directives: {
        "script-src": [
          "'self'",
          "https://cdn.jsdelivr.net",
          "https://unpkg.com",
        ],
        "style-src": ["'self'", "https://fonts.googleapis.com", "'unsafe-inline'"],
        "font-src": ["'self'", "https://fonts.gstatic.com"],
        "connect-src": ["'self'", "ws:", "wss:", "https://unpkg.com", "https://cdn.jsdelivr.net"],
        "img-src": ["'self'", "data:"],
      },
    },
  }),
);
app.use(express.json({ limit: "256kb" }));

function cookieSecure() {
  return (
    process.env.COOKIE_SECURE === "1" || process.env.NODE_ENV === "production"
  );
}

const sessionMiddleware = cookieSession({
  name: "solarstats_session",
  keys: [SESSION_SECRET],
  maxAge: LONG_COOKIE_MS,
  httpOnly: true,
  sameSite: "lax",
  secure: cookieSecure(),
});
app.use(sessionMiddleware);

function parseCookies(req) {
  const header = req.headers.cookie || "";
  const out = {};
  for (const part of header.split(";")) {
    const idx = part.indexOf("=");
    if (idx === -1) continue;
    const k = part.slice(0, idx).trim();
    const v = part.slice(idx + 1).trim();
    if (!k) continue;
    try {
      out[k] = decodeURIComponent(v);
    } catch {
      out[k] = v;
    }
  }
  return out;
}

function hasPasskeyCookie(req) {
  return parseCookies(req)[HAS_PASSKEY_COOKIE] === "1";
}

/** First-party only: HttpOnly + SameSite=Lax (+ Secure on HTTPS). Other sites cannot read it. */
function setHasPasskeyCookie(res) {
  const parts = [
    `${HAS_PASSKEY_COOKIE}=1`,
    "Path=/",
    `Max-Age=${Math.floor(LONG_COOKIE_MS / 1000)}`,
    "HttpOnly",
    "SameSite=Lax",
  ];
  if (cookieSecure()) parts.push("Secure");
  res.append("Set-Cookie", parts.join("; "));
}

function ensureHasPasskeyCookie(req, res, user) {
  if (!user) return;
  if (listPasskeysForUser(db, user.id).length > 0) {
    setHasPasskeyCookie(res);
  }
}

function currentUser(req) {
  if (!req.session?.userId) return null;
  return getUserById(db, req.session.userId);
}

function setSessionUser(req, user) {
  req.session.userId = user.id;
  req.session.email = user.email;
  req.session.role = user.role;
  req.session.status = user.status;
}

function clearSession(req) {
  req.session = null;
}

function requireApproved(req, res, next) {
  const user = currentUser(req);
  if (!user) {
    if (req.path.startsWith("/api/")) {
      return res.status(401).json({ error: "unauthorized" });
    }
    return res.redirect("/login");
  }
  if (user.status === "pending") {
    if (req.path.startsWith("/api/")) {
      return res.status(403).json({ error: "pending_approval" });
    }
    return res.redirect("/pending");
  }
  if (user.status !== "approved") {
    if (req.path.startsWith("/api/")) {
      return res.status(403).json({ error: "denied" });
    }
    return res.redirect("/login?error=denied");
  }
  req.user = user;
  return next();
}

function requireAdmin(req, res, next) {
  requireApproved(req, res, () => {
    if (!isAdminEmail(req.user.email)) {
      return res.status(403).json({ error: "admin_only" });
    }
    return next();
  });
}

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

// --- Public auth pages / routes ---

app.get("/login", (req, res) => {
  const user = currentUser(req);
  if (user?.status === "approved") return res.redirect("/");
  if (user?.status === "pending") return res.redirect("/pending");

  const hasPk = hasPasskeyCookie(req);
  const template = fs.readFileSync(path.join(publicDir, "login.html"), "utf8");
  const html = template.replaceAll("{{HAS_PASSKEY}}", hasPk ? "1" : "0");
  res.type("html").send(html);
});

app.get("/pending", (req, res) => {
  const user = currentUser(req);
  if (!user) return res.redirect("/login");
  if (user.status === "approved") return res.redirect("/");
  if (user.status === "denied") return res.redirect("/login?error=denied");
  res.sendFile(path.join(publicDir, "pending.html"));
});

app.get("/admin", requireAdmin, (_req, res) => {
  res.sendFile(path.join(publicDir, "admin.html"));
});

async function finishOidcLogin(provider, req, res) {
  try {
    const profile = await handleAuthCallback(provider, req, req.session);
    const { user, outcome } = upsertMicrosoftUser(db, {
      email: profile.email,
      displayName: profile.displayName,
    });

    if (outcome === "registration_closed") {
      clearSession(req);
      return res.redirect("/login?error=registration_closed");
    }
    if (!user || outcome === "denied") {
      clearSession(req);
      return res.redirect("/login?error=denied");
    }

    setSessionUser(req, user);
    if (user.status === "pending") return res.redirect("/pending");

    ensureHasPasskeyCookie(req, res, user);

    const afterLogin = req.session.afterLogin;
    delete req.session.afterLogin;
    if (afterLogin === "create_passkey") {
      return res.redirect("/setup-passkey");
    }
    return res.redirect("/");
  } catch (err) {
    console.error(`${provider} callback failed:`, err);
    clearSession(req);
    return res.redirect("/login?error=auth_callback");
  }
}

// Callbacks before /auth/:provider so "/auth/callback" is not swallowed.
app.get("/auth/callback/microsoft", (req, res) => finishOidcLogin("microsoft", req, res));
app.get("/auth/callback/google", (req, res) => finishOidcLogin("google", req, res));
app.get("/auth/callback/apple", (req, res) => finishOidcLogin("apple", req, res));
app.post(
  "/auth/callback/apple",
  express.urlencoded({ extended: false }),
  (req, res) => {
    const q = new URLSearchParams({ ...req.query, ...req.body });
    req.url = `/auth/callback/apple?${q.toString()}`;
    return finishOidcLogin("apple", req, res);
  },
);
app.get("/auth/callback", (req, res) => finishOidcLogin("microsoft", req, res));

app.get("/auth/:provider", async (req, res) => {
  const provider = String(req.params.provider || "");
  if (!["microsoft", "google", "apple"].includes(provider)) {
    return res.status(404).send("Unknown provider");
  }
  try {
    const providers = authProviders();
    if (!providers[provider]) {
      return res
        .status(503)
        .send(`${provider} sign-in is not configured on the server.`);
    }
    if (req.query.intent === "create_passkey") {
      req.session.afterLogin = "create_passkey";
    }
    const url = await buildAuthUrl(provider, req.session);
    res.redirect(url.href);
  } catch (err) {
    console.error(`${provider} auth start failed:`, err);
    res.redirect("/login?error=auth_start");
  }
});

app.get("/create-passkey", (req, res) => {
  res.sendFile(path.join(publicDir, "create-passkey.html"));
});

app.get("/setup-passkey", requireApproved, (_req, res) => {
  res.sendFile(path.join(publicDir, "setup-passkey.html"));
});

app.get("/api/auth/providers", (_req, res) => {
  res.json({ configured: authConfigured(), providers: authProviders() });
});

app.post("/logout", (req, res) => {
  clearSession(req);
  res.redirect("/login");
});

app.get("/api/me", (req, res) => {
  const user = currentUser(req);
  if (!user) return res.status(401).json({ error: "unauthorized" });
  const settings = getAuthSettings(db);
  // Existing users who already have a passkey but no marker cookie yet.
  if (!hasPasskeyCookie(req)) {
    ensureHasPasskeyCookie(req, res, user);
  } else if (listPasskeysForUser(db, user.id).length > 0) {
    // Refresh Max-Age while they use the site.
    setHasPasskeyCookie(res);
  }
  res.json({
    id: user.id,
    email: user.email,
    role: user.role,
    status: user.status,
    displayName: user.display_name,
    isAdmin: isAdminEmail(user.email),
    hasPasskeyCookie: hasPasskeyCookie(req) || listPasskeysForUser(db, user.id).length > 0,
    settings: {
      allowPasskeyEnrollment: settings.allowPasskeyEnrollment,
    },
    passkeys: listPasskeysForUser(db, user.id).map((p) => ({
      id: p.id,
      createdAt: p.created_at,
    })),
    webauthn: webauthnConfig(),
    authConfigured: authConfigured(),
  });
});

// --- Passkeys ---

/** Public: email + device passkey (no Google/Apple OAuth). */
app.post("/auth/passkey/enroll/options", async (req, res) => {
  try {
    const email = String(req.body?.email || "")
      .trim()
      .toLowerCase();
    if (!email || !email.includes("@")) {
      return res.status(400).json({ error: "Valid email required" });
    }

    const existing = getUserByEmail(db, email);
    // Don't let strangers attach a passkey to an approved account that already has one.
    // First passkey is allowed even when approved (bootstrap without OAuth).
    if (existing?.status === "approved") {
      const passkeys = listPasskeysForUser(db, existing.id);
      const loggedIn = currentUser(req);
      if (passkeys.length > 0 && (!loggedIn || loggedIn.id !== existing.id)) {
        return res.status(403).json({
          error:
            "This account already has a passkey. Sign in with passkey or MS Authenticator, then add another from the dashboard.",
        });
      }
    }

    const { user, outcome } = upsertMicrosoftUser(db, { email, displayName: null });
    if (outcome === "registration_closed" || !user) {
      return res.status(403).json({ error: "New account registration is closed" });
    }
    if (user.status === "denied") {
      return res.status(403).json({ error: "Account access denied" });
    }

    const options = await registrationOptions(db, user, { allowPending: true });
    req.session.passkeyChallenge = options.challenge;
    req.session.passkeyEnrollUserId = user.id;
    res.json({
      options,
      status: user.status,
      email: user.email,
    });
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.post("/auth/passkey/enroll/verify", async (req, res) => {
  try {
    const challenge = req.session.passkeyChallenge;
    const userId = req.session.passkeyEnrollUserId;
    if (!challenge || !userId) {
      return res.status(400).json({ error: "missing enrollment session" });
    }
    const user = getUserById(db, userId);
    if (!user) return res.status(400).json({ error: "unknown user" });

    const result = await verifyRegistration(db, user, req.body, challenge, {
      allowPending: true,
    });
    delete req.session.passkeyChallenge;
    delete req.session.passkeyEnrollUserId;

    setSessionUser(req, user);
    setHasPasskeyCookie(res);
    res.json({
      ...result,
      status: user.status,
      email: user.email,
      redirect: user.status === "approved" ? "/" : "/pending",
    });
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.post("/auth/passkey/register/options", requireApproved, async (req, res) => {
  try {
    const options = await registrationOptions(db, req.user);
    req.session.passkeyChallenge = options.challenge;
    res.json(options);
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.post("/auth/passkey/register/verify", requireApproved, async (req, res) => {
  try {
    const challenge = req.session.passkeyChallenge;
    if (!challenge) {
      return res.status(400).json({ error: "missing challenge" });
    }
    const result = await verifyRegistration(db, req.user, req.body, challenge);
    delete req.session.passkeyChallenge;
    setHasPasskeyCookie(res);
    res.json(result);
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.post("/auth/passkey/login/options", async (req, res) => {
  try {
    const options = await authenticationOptions(db);
    req.session.passkeyChallenge = options.challenge;
    res.json(options);
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.post("/auth/passkey/login/verify", async (req, res) => {
  try {
    const challenge = req.session.passkeyChallenge;
    if (!challenge) {
      return res.status(400).json({ error: "missing challenge" });
    }
    const { user } = await verifyAuthentication(db, req.body, challenge);
    delete req.session.passkeyChallenge;
    setSessionUser(req, user);
    setHasPasskeyCookie(res);
    res.json({ ok: true, email: user.email });
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

app.delete("/auth/passkey/:id", requireApproved, (req, res) => {
  const id = Number(req.params.id);
  const changes = deletePasskey(db, id, req.user.id);
  if (!changes) return res.status(404).json({ error: "not_found" });
  res.json({ ok: true });
});

// --- Admin API ---

app.get("/api/admin/users", requireAdmin, (_req, res) => {
  res.json({
    users: listUsers(db),
    settings: getAuthSettings(db),
  });
});

app.post("/api/admin/users/:id/status", requireAdmin, (req, res) => {
  const id = Number(req.params.id);
  const status = String(req.body?.status || "");
  if (!["approved", "denied", "pending"].includes(status)) {
    return res.status(400).json({ error: "invalid_status" });
  }
  const target = getUserById(db, id);
  if (!target) return res.status(404).json({ error: "not_found" });
  if (target.role === "admin" && status !== "approved") {
    return res.status(400).json({ error: "cannot_demote_admin" });
  }
  const user = setUserStatus(db, id, status);
  res.json({ user });
});

app.post("/api/admin/settings", requireAdmin, (req, res) => {
  const settings = setAuthSettings(db, {
    allowNewAccounts: req.body?.allowNewAccounts,
    allowPasskeyEnrollment: req.body?.allowPasskeyEnrollment,
  });
  res.json({ settings });
});

// --- Protected dashboard / data ---

app.get("/", requireApproved, (_req, res) => {
  res.sendFile(path.join(publicDir, "solarstats.html"));
});

app.get("/solarstats", (_req, res) => {
  res.redirect(301, "/");
});

app.get("/api/history", requireApproved, (req, res) => {
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
  res.json({
    ok: true,
    authConfigured: authConfigured(),
  });
});

app.use(express.static(publicDir));

const server = http.createServer(app);
const wss = new WebSocketServer({ noServer: true });

function broadcast(message) {
  const data = JSON.stringify(message);
  for (const client of wss.clients) {
    if (client.readyState === 1) {
      client.send(data);
    }
  }
}

function runSession(req, res, next) {
  sessionMiddleware(req, res, next);
}

server.on("upgrade", (req, socket, head) => {
  if (req.url?.split("?")[0] !== "/ws") {
    socket.destroy();
    return;
  }

  const res = new http.ServerResponse(req);
  runSession(req, res, () => {
    const userId = req.session?.userId;
    const user = userId ? getUserById(db, userId) : null;
    if (!user || user.status !== "approved") {
      socket.write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
      socket.destroy();
      return;
    }

    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit("connection", ws, req);
    });
  });
});

wss.on("connection", (socket) => {
  const history = getHistory(db, "1h");
  socket.send(
    JSON.stringify({
      type: "hello",
      energyKwhTotal: history.energyKwhTotal,
      latest: history.latest,
      loadsDailyKwh: history.loadsDailyKwh,
    }),
  );
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
  console.log(
    authConfigured()
      ? "Microsoft auth: configured"
      : "Microsoft auth: NOT configured (set AZURE_* / AUTH_REDIRECT_URI / SESSION_SECRET)",
  );
  console.log(`WebAuthn RP: ${JSON.stringify(webauthnConfig())}`);
  console.log(
    getAdminEmail()
      ? `Admin email: ${getAdminEmail()}`
      : "Admin email: NOT set (ADMIN_EMAIL in .env)",
  );
});
