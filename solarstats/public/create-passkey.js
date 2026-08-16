import {
  startRegistration,
} from "https://unpkg.com/@simplewebauthn/browser@13.1.0/esm/index.js";

const errorBox = document.getElementById("errorBox");
const createBtn = document.getElementById("createBtn");
const emailInput = document.getElementById("email");

function showError(msg) {
  errorBox.hidden = false;
  errorBox.textContent = msg;
}

createBtn.addEventListener("click", async () => {
  const email = emailInput.value.trim();
  if (!email.includes("@")) {
    showError("Enter a valid email address");
    return;
  }
  if (!window.PublicKeyCredential) {
    showError("Passkeys are not supported in this browser");
    return;
  }

  createBtn.disabled = true;
  errorBox.hidden = true;
  try {
    const optRes = await fetch("/auth/passkey/enroll/options", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ email }),
    });
    const optBody = await optRes.json().catch(() => ({}));
    if (!optRes.ok) throw new Error(optBody.error || "options failed");

    const credential = await startRegistration({ optionsJSON: optBody.options });
    const verifyRes = await fetch("/auth/passkey/enroll/verify", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(credential),
    });
    const verifyBody = await verifyRes.json().catch(() => ({}));
    if (!verifyRes.ok) throw new Error(verifyBody.error || "verify failed");

    location.href = verifyBody.redirect || "/pending";
  } catch (err) {
    showError(err.message || "Passkey creation failed");
    createBtn.disabled = false;
  }
});
