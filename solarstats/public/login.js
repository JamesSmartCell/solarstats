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
if (code && ERRORS[code]) {
  errorBox.hidden = false;
  errorBox.textContent = ERRORS[code];
}

const passkeyBtn = document.getElementById("passkeyBtn");
const passkeyHint = document.getElementById("passkeyHint");

if (!window.PublicKeyCredential) {
  passkeyBtn.disabled = true;
  passkeyBtn.textContent = "Passkeys not supported on this browser";
} else {
  passkeyHint.hidden = false;
}

passkeyBtn?.addEventListener("click", async () => {
  passkeyBtn.disabled = true;
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
    errorBox.hidden = false;
    errorBox.textContent =
      err.message ||
      "Passkey sign-in failed. If you have not created one yet, use Create passkey.";
    passkeyBtn.disabled = false;
  }
});
