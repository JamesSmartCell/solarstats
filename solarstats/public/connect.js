const form = document.getElementById("connectForm");
const errorBox = document.getElementById("errorBox");
const statusBox = document.getElementById("statusBox");

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  errorBox.hidden = true;
  statusBox.hidden = true;
  const code = document.getElementById("code").value;
  const res = await fetch("/api/pair/claim", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ code }),
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    errorBox.hidden = false;
    errorBox.textContent =
      body.error === "code_not_found"
        ? "That code is not active. Check the Home Assistant screen and try again."
        : body.error || "Could not connect";
    return;
  }
  statusBox.hidden = false;
  statusBox.textContent = body.signedIn
    ? `${body.name} is connected. Opening the dashboard…`
    : `${body.name} is connected. Sign in as ${body.email} to open it.`;
  if (body.signedIn) {
    location.href = body.path;
    return;
  }
  location.href = "/login";
});
