import {
  startRegistration,
} from "https://unpkg.com/@simplewebauthn/browser@13.1.0/esm/index.js";

const errorBox = document.getElementById("errorBox");
const okBox = document.getElementById("okBox");
const createBtn = document.getElementById("createBtn");
const emailEl = document.getElementById("email");

function showError(msg) {
  errorBox.hidden = false;
  errorBox.textContent = msg;
  okBox.hidden = true;
}

async function loadMe() {
  const res = await fetch("/api/me");
  if (res.status === 401) {
    location.href = "/login";
    return null;
  }
  if (!res.ok) throw new Error("Could not load account");
  return res.json();
}

createBtn.addEventListener("click", async () => {
  createBtn.disabled = true;
  errorBox.hidden = true;
  try {
    if (!window.PublicKeyCredential) {
      throw new Error("Passkeys are not supported in this browser");
    }
    const optRes = await fetch("/auth/passkey/register/options", { method: "POST" });
    if (!optRes.ok) {
      throw new Error((await optRes.json().catch(() => ({}))).error || "options failed");
    }
    const options = await optRes.json();
    const credential = await startRegistration({ optionsJSON: options });
    const verifyRes = await fetch("/auth/passkey/register/verify", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(credential),
    });
    if (!verifyRes.ok) {
      throw new Error((await verifyRes.json().catch(() => ({}))).error || "verify failed");
    }
    okBox.hidden = false;
    createBtn.textContent = "Passkey created";
    setTimeout(() => {
      location.href = "/";
    }, 1200);
  } catch (err) {
    showError(err.message || "Passkey creation failed");
    createBtn.disabled = false;
  }
});

loadMe()
  .then((me) => {
    if (!me) return;
    emailEl.textContent = me.email || "—";
    if ((me.passkeys || []).length > 0) {
      createBtn.textContent = "Add another passkey";
    }
  })
  .catch((err) => showError(err.message));
