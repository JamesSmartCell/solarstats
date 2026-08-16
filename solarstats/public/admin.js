async function load() {
  const res = await fetch("/api/admin/users");
  if (res.status === 401 || res.status === 403) {
    location.href = "/login";
    return;
  }
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = await res.json();
  renderSettings(data.settings);
  renderUsers(data.users);
}

function renderSettings(settings) {
  const allowNew = document.getElementById("allowNewAccounts");
  const allowPk = document.getElementById("allowPasskeyEnrollment");
  allowNew.checked = !!settings.allowNewAccounts;
  allowPk.checked = !!settings.allowPasskeyEnrollment;

  async function save() {
    const res = await fetch("/api/admin/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        allowNewAccounts: allowNew.checked,
        allowPasskeyEnrollment: allowPk.checked,
      }),
    });
    if (!res.ok) return;
    const note = document.getElementById("settingsSaved");
    note.hidden = false;
    setTimeout(() => {
      note.hidden = true;
    }, 1500);
  }

  allowNew.onchange = save;
  allowPk.onchange = save;
}

function renderUsers(users) {
  const tbody = document.querySelector("#usersTable tbody");
  tbody.innerHTML = "";
  for (const u of users) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escapeHtml(u.email)}</td>
      <td>${escapeHtml(u.role)}</td>
      <td><span class="status-pill ${escapeHtml(u.status)}">${escapeHtml(u.status)}</span></td>
      <td>${escapeHtml(formatDate(u.created_at))}</td>
      <td class="admin-actions"></td>
    `;
    const actions = tr.querySelector(".admin-actions");
    if (u.role !== "admin") {
      if (u.status !== "approved") {
        actions.appendChild(actionBtn("Approve", () => setStatus(u.id, "approved")));
      }
      if (u.status !== "denied") {
        actions.appendChild(actionBtn("Deny", () => setStatus(u.id, "denied")));
      }
      if (u.status === "approved") {
        actions.appendChild(actionBtn("Revoke", () => setStatus(u.id, "denied")));
      }
    } else {
      actions.textContent = "—";
    }
    tbody.appendChild(tr);
  }
}

function actionBtn(label, onClick) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.textContent = label;
  btn.addEventListener("click", onClick);
  return btn;
}

async function setStatus(id, status) {
  const res = await fetch(`/api/admin/users/${id}/status`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ status }),
  });
  if (!res.ok) {
    alert((await res.json().catch(() => ({}))).error || "Update failed");
    return;
  }
  load().catch(console.error);
}

function formatDate(iso) {
  if (!iso) return "—";
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
  }
}

function escapeHtml(s) {
  return String(s ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

load().catch((err) => {
  console.error(err);
  alert("Failed to load admin data");
});
