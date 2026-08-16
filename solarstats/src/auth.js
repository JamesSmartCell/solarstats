import * as oidc from "openid-client";

let cachedConfig = null;

export function authConfigured() {
  return Boolean(
    process.env.AZURE_CLIENT_ID &&
      process.env.AZURE_CLIENT_SECRET &&
      process.env.AUTH_REDIRECT_URI &&
      process.env.SESSION_SECRET,
  );
}

export async function getOidcConfig() {
  if (cachedConfig) return cachedConfig;
  if (!authConfigured()) {
    throw new Error("Microsoft auth is not configured");
  }

  const tenant = process.env.AZURE_TENANT || "consumers";
  const issuer = new URL(
    `https://login.microsoftonline.com/${tenant}/v2.0`,
  );

  cachedConfig = await oidc.discovery(
    issuer,
    process.env.AZURE_CLIENT_ID,
    process.env.AZURE_CLIENT_SECRET,
  );
  return cachedConfig;
}

export async function buildMicrosoftAuthUrl(session) {
  const config = await getOidcConfig();
  const codeVerifier = oidc.randomPKCECodeVerifier();
  const codeChallenge = await oidc.calculatePKCECodeChallenge(codeVerifier);
  const state = oidc.randomState();
  const nonce = oidc.randomNonce();

  session.oidc = {
    codeVerifier,
    state,
    nonce,
  };

  return oidc.buildAuthorizationUrl(config, {
    redirect_uri: process.env.AUTH_REDIRECT_URI,
    scope: "openid profile email",
    code_challenge: codeChallenge,
    code_challenge_method: "S256",
    state,
    nonce,
    response_type: "code",
  });
}

export async function handleMicrosoftCallback(req, session) {
  const config = await getOidcConfig();
  const pending = session.oidc;
  if (!pending?.codeVerifier || !pending?.state) {
    throw new Error("missing OIDC session state");
  }

  const currentUrl = new URL(
    `${req.protocol}://${req.get("host")}${req.originalUrl}`,
  );
  // Prefer configured public origin so reverse-proxy host quirks don't break token exchange.
  const redirectBase = process.env.AUTH_REDIRECT_URI
    ? new URL(process.env.AUTH_REDIRECT_URI)
    : currentUrl;
  const callbackUrl = new URL(
    `${redirectBase.origin}${req.originalUrl}`,
  );

  const tokens = await oidc.authorizationCodeGrant(config, callbackUrl, {
    pkceCodeVerifier: pending.codeVerifier,
    expectedState: pending.state,
    expectedNonce: pending.nonce,
  });

  delete session.oidc;

  const claims = tokens.claims() || {};
  const email = String(
    claims.email || claims.preferred_username || "",
  )
    .trim()
    .toLowerCase();
  const displayName = claims.name ? String(claims.name) : null;

  if (!email) {
    throw new Error("Microsoft account did not return an email claim");
  }

  return { email, displayName, sub: claims.sub };
}

export function webauthnConfig() {
  const origin = (process.env.ORIGIN || "").replace(/\/$/, "");
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
