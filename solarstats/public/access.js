const applyBtn = document.getElementById("apply");
const status = document.getElementById("status");

function showSent() {
  if (applyBtn) applyBtn.hidden = true;
  if (!status) return;
  status.hidden = false;
  status.textContent =
    "Request sent. The admin of this home will see it the next time they open the admin page.";
}

if (document.body.dataset.applied === "1") showSent();

applyBtn?.addEventListener("click", async () => {
  applyBtn.disabled = true;
  if (status) status.hidden = true;
  try {
    const res = await fetch("/api/access-requests", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ slug: document.body.dataset.slug || "" }),
    });
    if (!res.ok) throw new Error("request_failed");
    showSent();
  } catch {
    applyBtn.disabled = false;
    if (status) {
      status.hidden = false;
      status.textContent = "Could not send the request. Try again.";
    }
  }
});
