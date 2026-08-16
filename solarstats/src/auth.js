import * as oidc from "openid-client";
import fs from "node:fs";
import crypto from "node:crypto";

const configs = new Map();

export function microsoftConfigured() {
  return Boolean(
    process.env.AZURE_CLIENT_ID &&
      process.env.AZURE_CLIENT_SECRET &&
      microsoftRedirectUri() &&
      process.env.SESSION_SECRET,
  );
}

export function googleConfigured() {
  return Boolean(
    process.env.GOOGLE_CLIENT_ID &&
      process.env.GOOGLE_CLIENT_SECRET &&
      googleRedirectUri() &&
      process.env.SESSION_SECRET,
  );
}

export function appleConfigured() {
  return Boolean(
    process.env.APPLE_CLIENT_ID &&
      process.env.APPLE_TEAM_ID &&
      process.env.APPLE_KEY_ID &&
      applePrivateKey() &&
      appleRedirectUri() &&
      process.env.SESSION_SECRET,
  );
}

/** True if at least one IdP is ready. */
export function authConfigured() {
  return microsoftConfigured() || googleConfigured() || appleConfigured();
}

export function authProviders() {
  return {
    microsoft: microsoftConfigured(),
    google: googleConfigured(),
    apple: appleConfigured(),
  };
}

function originBase() {
  return (process.env.ORIGIN || "").replace(/\/$/, "");
}

export function microsoftRedirectUri() {
  return (
    process.env.AUTH_REDIRECT_URI ||
    (originBase() ? `${originBase()}/auth/callback/microsoft` : "")
  );
}

export function googleRedirectUri() {
  return (
    process.env.GOOGLE_REDIRECT_URI ||
    (originBase() ? `${originBase()}/auth/callback/google` : "")
  );
}

export function appleRedirectUri() {
  return (
    process.env.APPLE_REDIRECT_URI ||
    (originBase() ? `${originBase()}/auth/callback/apple` : "")
  );
}

function applePrivateKey() {
  if (process.env.APPLE_PRIVATE_KEY) {
    return process.env.APPLE_PRIVATE_KEY.replace(/\\n/g, "\n");
  }
  if (process.env.APPLE_PRIVATE_KEY_PATH) {
    try {
      return fs.readFileSync(process.env.APPLE_PRIVATE_KEY_PATH, "utf8");
    } catch {
      return "";
    }
  }
  return "";
}

/** Apple requires a short-lived JWT as client_secret. */
function appleClientSecret() {
  const teamId = process.env.APPLE_TEAM_ID;
  const clientId = process.env.APPLE_CLIENT_ID;
  const keyId = process.env.APPLE_KEY_ID;
  const key = applePrivateKey();
  const now = Math.floor(Date.now() / 1000);
  const header = { alg: "ES256", kid: keyId };
  const payload = {
    iss: teamId,
    iat: now,
    exp: now + 86400 * 150,
    aud: "https://appleid.apple.com",
    sub: clientId,
  };
  return signJwtEs256(header, payload, key);
}

function b64url(input) {
  return Buffer.from(input)
    .toString("base64")
    .replace(/=/g, "")
    .replace(/\+/g, "-")
    .replace(/\//g, "_");
}

function signJwtEs256(header, payload, pemKey) {
  const data = `${b64url(JSON.stringify(header))}.${b64url(JSON.stringify(payload))}`;
  const key = crypto.createPrivateKey(pemKey);
  const sig = crypto.sign("sha256", Buffer.from(data), {
    key,
    dsaEncoding: "ieee-p1363",
  });
  return `${data}.${sig.toString("base64").replace(/=/g, "").replace(/\+/g, "-").replace(/\//g, "_")}`;
}

async function getProviderConfig(provider) {
  if (configs.has(provider)) return configs.get(provider);

  let config;
  if (provider === "microsoft") {
    if (!microsoftConfigured()) throw new Error("Microsoft auth is not configured");
    const tenant = process.env.AZURE_TENANT || "consumers";
    config = await oidc.discovery(
      new URL(`https://login.microsoftonline.com/${tenant}/v2.0`),
      process.env.AZURE_CLIENT_ID,
      process.env.AZURE_CLIENT_SECRET,
    );
  } else if (provider === "google") {
    if (!googleConfigured()) throw new Error("Google auth is not configured");
    config = await oidc.discovery(
      new URL("https://accounts.google.com"),
      process.env.GOOGLE_CLIENT_ID,
      process.env.GOOGLE_CLIENT_SECRET,
    );
  } else if (provider === "apple") {
    if (!appleConfigured()) throw new Error("Apple auth is not configured");
    config = await oidc.discovery(
      new URL("https://appleid.apple.com"),
      process.env.APPLE_CLIENT_ID,
      appleClientSecret(),
    );
  } else {
    throw new Error(`Unknown provider: ${provider}`);
  }

  configs.set(provider, config);
  return config;
}

function redirectUriFor(provider) {
  if (provider === "microsoft") return microsoftRedirectUri();
  if (provider === "google") return googleRedirectUri();
  if (provider === "apple") return appleRedirectUri();
  throw new Error(`Unknown provider: ${provider}`);
}

export async function buildAuthUrl(provider, session) {
  // Apple client_secret expires — refresh config each time for Apple.
  if (provider === "apple") configs.delete("apple");

  const config = await getProviderConfig(provider);
  const codeVerifier = oidc.randomPKCECodeVerifier();
  const codeChallenge = await oidc.calculatePKCECodeChallenge(codeVerifier);
  const state = oidc.randomState();
  const nonce = oidc.randomNonce();

  session.oidc = {
    provider,
    codeVerifier,
    state,
    nonce,
  };

  const params = {
    redirect_uri: redirectUriFor(provider),
    scope: provider === "apple" ? "openid name email" : "openid profile email",
    code_challenge: codeChallenge,
    code_challenge_method: "S256",
    state,
    nonce,
    response_type: "code",
  };

  if (provider === "google") {
    params.access_type = "online";
    params.prompt = "select_account";
  }

  return oidc.buildAuthorizationUrl(config, params);
}

export async function handleAuthCallback(provider, req, session) {
  if (provider === "apple") configs.delete("apple");

  const config = await getProviderConfig(provider);
  const pending = session.oidc;
  if (!pending?.codeVerifier || !pending?.state || pending.provider !== provider) {
    throw new Error("missing OIDC session state");
  }

  const redirectBase = new URL(redirectUriFor(provider));
  const callbackUrl = new URL(`${redirectBase.origin}${req.originalUrl}`);

  // Apple may POST form_post; express already parsed query for GET.
  const tokens = await oidc.authorizationCodeGrant(config, callbackUrl, {
    pkceCodeVerifier: pending.codeVerifier,
    expectedState: pending.state,
    expectedNonce: pending.nonce,
  });

  delete session.oidc;

  const claims = tokens.claims() || {};
  let email = String(claims.email || claims.preferred_username || "")
    .trim()
    .toLowerCase();

  // Apple sometimes omits email on later sign-ins; keep sub-based fallback only if needed.
  if (!email && provider === "apple" && claims.sub) {
    email = `apple.${claims.sub}@privaterelay.appleid.com`.toLowerCase();
  }

  const displayName = claims.name
    ? typeof claims.name === "string"
      ? claims.name
      : [claims.name.firstName, claims.name.lastName].filter(Boolean).join(" ")
    : null;

  if (!email) {
    throw new Error(`${provider} account did not return an email claim`);
  }

  return {
    email,
    displayName,
    sub: claims.sub,
    provider,
  };
}

// Back-compat exports used by older call sites
export async function buildMicrosoftAuthUrl(session) {
  return buildAuthUrl("microsoft", session);
}

export async function handleMicrosoftCallback(req, session) {
  return handleAuthCallback("microsoft", req, session);
}

export function webauthnConfig() {
  const origin = originBase();
  let rpID = process.env.RP_ID || "";
  if (!rpID && origin) {
    try {
      rpID = new URL(origin).hostname;
    } catch {
      rpID = "localhost";
    }
  }
  if (!rpID) rpID = "localhost";
  return {
    rpID,
    rpName: "Solarstats",
    origin: origin || `http://${rpID}:8787`,
  };
}
