import { renderProviderButtons } from "./providers.js";

const errorBox = document.getElementById("errorBox");

renderProviderButtons(document.getElementById("providerButtons"), {
  intent: "create_passkey",
}).catch((err) => {
  errorBox.hidden = false;
  errorBox.textContent = err.message || "Could not load sign-in providers";
});
