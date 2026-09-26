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
    if (body.error === "sign_in_required") {
      location.href = "/login";
      return;
    }
    errorBox.textContent =
      body.error === "code_not_found"
        ? "That code is not active. Check the Home Assistant screen and try again."
        : body.error || "Could not connect";
    return;
  }
  statusBox.hidden = false;
  statusBox.textContent = `${body.name} is connected. You are its admin. Opening the dashboard…`;
  location.href = body.path;
});
