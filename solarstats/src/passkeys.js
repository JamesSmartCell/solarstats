import {
  generateAuthenticationOptions,
  generateRegistrationOptions,
  verifyAuthenticationResponse,
  verifyRegistrationResponse,
} from "@simplewebauthn/server";
import {
  getAuthSettings,
  getPasskeyByCredentialId,
  getUserById,
  insertPasskey,
  listPasskeysForUser,
  updatePasskeyCounter,
} from "./db.js";
import { webauthnConfig } from "./auth.js";

function b64urlToBuffer(value) {
  return Buffer.from(value, "base64url");
}

function bufferToB64url(buf) {
  return Buffer.from(buf).toString("base64url");
}

export async function registrationOptions(db, user, { allowPending = false } = {}) {
  if (user.status === "denied") {
    const err = new Error("Account access denied");
    err.status = 403;
    throw err;
  }
  if (user.status === "pending" && !allowPending) {
    const err = new Error("Account not approved");
    err.status = 403;
    throw err;
  }
  if (user.status !== "approved" && user.status !== "pending") {
    const err = new Error("Account not approved");
    err.status = 403;
    throw err;
  }

  const existing = listPasskeysForUser(db, user.id);
  const settings = getAuthSettings(db);
  // First passkey is always allowed; further ones need the admin toggle.
  if (!settings.allowPasskeyEnrollment && existing.length > 0) {
    const err = new Error("Passkey enrollment is disabled");
    err.status = 403;
    throw err;
  }

  const { rpID, rpName } = webauthnConfig();

  const options = await generateRegistrationOptions({
    rpName,
    rpID,
    userName: user.email,
    userDisplayName: user.display_name || user.email,
    userID: new TextEncoder().encode(String(user.id)),
    attestationType: "none",
    excludeCredentials: existing.map((pk) => ({
      id: pk.credential_id,
      transports: pk.transports ? JSON.parse(pk.transports) : undefined,
    })),
    authenticatorSelection: {
      // Prefer this device (Apple Passwords / Google PM / Windows Hello), not roaming Microsoft Authenticator.
      authenticatorAttachment: "platform",
      residentKey: "preferred",
      userVerification: "preferred",
    },
  });

  return options;
}

export async function verifyRegistration(
  db,
  user,
  response,
  expectedChallenge,
  { allowPending = false } = {},
) {
  if (user.status === "denied") {
    const err = new Error("Account access denied");
    err.status = 403;
    throw err;
  }
  if (user.status === "pending" && !allowPending) {
    const err = new Error("Account not approved");
    err.status = 403;
    throw err;
  }
  if (user.status !== "approved" && user.status !== "pending") {
    const err = new Error("Account not approved");
    err.status = 403;
    throw err;
  }

  const existing = listPasskeysForUser(db, user.id);
  const settings = getAuthSettings(db);
  if (!settings.allowPasskeyEnrollment && existing.length > 0) {
    const err = new Error("Passkey enrollment is disabled");
    err.status = 403;
    throw err;
  }

  const { rpID, origin } = webauthnConfig();
  const verification = await verifyRegistrationResponse({
    response,
    expectedChallenge,
    expectedOrigin: origin,
    expectedRPID: rpID,
    requireUserVerification: false,
  });

  if (!verification.verified || !verification.registrationInfo) {
    const err = new Error("Passkey registration failed");
    err.status = 400;
    throw err;
  }

  const { credential, credentialDeviceType, credentialBackedUp } =
    verification.registrationInfo;

  insertPasskey(db, {
    userId: user.id,
    credentialId: credential.id,
    publicKey: bufferToB64url(credential.publicKey),
    counter: credential.counter,
    transports: response.response?.transports || credential.transports,
  });

  return {
    verified: true,
    credentialDeviceType,
    credentialBackedUp,
  };
}

export async function authenticationOptions(db) {
  const { rpID } = webauthnConfig();
  const options = await generateAuthenticationOptions({
    rpID,
    userVerification: "preferred",
  });
  return options;
}

export async function verifyAuthentication(db, response, expectedChallenge) {
  const { rpID, origin } = webauthnConfig();
  const credentialId = response.id;
  const stored = getPasskeyByCredentialId(db, credentialId);
  if (!stored) {
    const err = new Error("Unknown passkey");
    err.status = 400;
    throw err;
  }

  const user = getUserById(db, stored.user_id);
  if (!user || user.status !== "approved") {
    const err = new Error("Account not approved");
    err.status = 403;
    throw err;
  }

  const verification = await verifyAuthenticationResponse({
    response,
    expectedChallenge,
    expectedOrigin: origin,
    expectedRPID: rpID,
    requireUserVerification: false,
    credential: {
      id: stored.credential_id,
      publicKey: b64urlToBuffer(stored.public_key),
      counter: stored.counter,
      transports: stored.transports ? JSON.parse(stored.transports) : undefined,
    },
  });

  if (!verification.verified) {
    const err = new Error("Passkey authentication failed");
    err.status = 400;
    throw err;
  }

  updatePasskeyCounter(
    db,
    stored.credential_id,
    verification.authenticationInfo.newCounter,
  );

  return { user, verification };
}
