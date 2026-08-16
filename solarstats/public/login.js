import {
  startAuthentication,
} from "https://unpkg.com/@simplewebauthn/browser@13.1.0/esm/index.js";

const ERRORS = {
  registration_closed: "New account registration is currently closed.",
  denied: "Your account access was denied.",
  auth_start: "Could not start Microsoft sign-in. Check server configuration.",
  auth_callback: "Microsoft sign-in failed. Try again.",
};

const params = new URLSearchParams(location.search);
const code = params.get("error");
const errorBox = document.getElementById("errorBox");
const statusBox = document.getElementById("statusBox");
const choices = document.getElementById("choices");
const passkeyBtn = document.getElementById("passkeyBtn");

const hasPasskey = document.body.dataset.hasPasskey === "1";

if (code && ERRORS[code]) {
  errorBox.hidden = false;
  errorBox.textContent = ERRORS[code];
}

function showChoices() {
  statusBox.hidden = true;
  choices.hidden = false;
}

async function signInWithPasskey({ silent = false } = {}) {
  if (!window.PublicKeyCredential) {
    if (!silent) {
      errorBox.hidden = false;
      errorBox.textContent = "Passkeys are not supported in this browser";
    }
    showChoices();
    return;
  }

  if (passkeyBtn) passkeyBtn.disabled = true;
  if (silent) {
    statusBox.hidden = false;
    choices.hidden = true;
  }

  try {
    const optRes = await fetch("/auth/passkey/login/options", { method: "POST" });
    if (!optRes.ok) {
      throw new Error((await optRes.json().catch(() => ({}))).error || "options failed");
    }
    const options = await optRes.json();
    const credential = await startAuthentication({ optionsJSON: options });
    const verifyRes = await fetch("/auth/passkey/login/verify", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(credential),
    });
    if (!verifyRes.ok) {
      throw new Error((await verifyRes.json().catch(() => ({}))).error || "verify failed");
    }
    location.href = "/";
  } catch (err) {
    if (!silent) {
      errorBox.hidden = false;
      errorBox.textContent = err.message || "Passkey sign-in failed";
    }
    if (passkeyBtn) passkeyBtn.disabled = false;
    showChoices();
  }
}

passkeyBtn?.addEventListener("click", () => signInWithPasskey({ silent: false }));

if (hasPasskey && !code) {
  // Session gone, but this browser previously enrolled a passkey — prompt immediately.
  signInWithPasskey({ silent: true });
} else {
  showChoices();
}
